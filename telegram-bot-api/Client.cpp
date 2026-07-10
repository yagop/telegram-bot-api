//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/Client.h"

#include "telegram-bot-api/ClientParameters.h"

#include "td/db/TQueue.h"

#include "td/actor/MultiPromise.h"
#include "td/actor/SleepActor.h"

#include "td/utils/algorithm.h"
#include "td/utils/base64.h"
#include "td/utils/emoji.h"
#include "td/utils/filesystem.h"
#include "td/utils/HttpUrl.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/PathView.h"
#include "td/utils/port/config.h"
#include "td/utils/port/path.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/Span.h"
#include "td/utils/StackAllocator.h"
#include "td/utils/Status.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/Time.h"
#include "td/utils/utf8.h"

#include <cstdlib>
#include <utility>

namespace telegram_bot_api {

using td_api::make_object;
using td_api::move_object_as;

Client::Client(td::ActorShared<> parent, const td::string &bot_token, bool is_test_dc, int64 tqueue_id,
               std::shared_ptr<const ClientParameters> parameters, td::ActorId<BotStatActor> stat_actor)
    : parent_(std::move(parent))
    , bot_token_(bot_token)
    , bot_token_id_("<unknown>")
    , is_test_dc_(is_test_dc)
    , tqueue_id_(tqueue_id)
    , parameters_(std::move(parameters))
    , stat_actor_(std::move(stat_actor)) {
  static auto is_inited = init_methods();
  CHECK(is_inited);
}

Client::~Client() {
  td::Scheduler::instance()->destroy_on_scheduler(SharedData::get_file_gc_scheduler_id(), messages_, users_, groups_,
                                                  supergroups_, chats_, communities_, sticker_set_names_);
}

int Client::get_retry_after_time(td::Slice error_message) {
  td::Slice prefix = "Too Many Requests: retry after ";
  if (td::begins_with(error_message, prefix)) {
    auto r_retry_after = td::to_integer_safe<int>(error_message.substr(prefix.size()));
    if (r_retry_after.is_ok() && r_retry_after.ok() > 0) {
      return r_retry_after.ok();
    }
  }
  return 0;
}

void Client::fail_query_with_error(PromisedQueryPtr query, int32 error_code, td::CSlice error_message,
                                   td::CSlice default_message) {
  if (error_code == 429) {
    auto retry_after_time = get_retry_after_time(error_message);
    if (retry_after_time > 0) {
      return query->set_retry_after_error(retry_after_time);
    }
    LOG(ERROR) << "Wrong error message: " << error_message << " from " << *query;
    return fail_query(500, error_message, std::move(query));
  }
  int32 real_error_code = error_code;
  td::Slice real_error_message = error_message;
  if (error_code < 400 || error_code == 404) {
    if (error_code < 200) {
      LOG(ERROR) << "Receive error \"" << real_error_message << "\" with code " << error_code << " from " << *query;
    }

    error_code = 400;
  } else if (error_code == 403) {
    bool is_server_error = true;
    for (auto c : error_message) {
      if (c == '_' || ('A' <= c && c <= 'Z') || td::is_digit(c)) {
        continue;
      }

      is_server_error = false;
      break;
    }
    if (is_server_error) {
      error_code = 400;
    }
  }
  td::string error_buf;
  if (error_code == 400) {
    if (!default_message.empty()) {
      error_message = default_message;
    }
    if (error_message == "MESSAGE_NOT_MODIFIED") {
      error_message =
          "message is not modified: specified new message content and reply markup are exactly the same as a current "
          "content and reply markup of the message";
    } else if (error_message == "WC_CONVERT_URL_INVALID" || error_message == "EXTERNAL_URL_INVALID") {
      error_message = "Wrong HTTP URL specified";
    } else if (error_message == "WEBPAGE_CURL_FAILED") {
      error_message = "Failed to get HTTP URL content";
    } else if (error_message == "WEBPAGE_MEDIA_EMPTY") {
      error_message = "Wrong type of the web page content";
    } else if (error_message == "MEDIA_GROUPED_INVALID") {
      error_message = "Can't use the media of the specified type in the album";
    } else if (error_message == "REPLY_MARKUP_TOO_LONG") {
      error_message = "reply markup is too long";
    } else if (error_message == "INPUT_USER_DEACTIVATED") {
      error_code = 403;
      error_message = "Forbidden: user is deactivated";
    } else if (error_message == "USER_IS_BLOCKED") {
      error_code = 403;
      error_message = "bot was blocked by the user";
    } else if (error_message == "USER_ADMIN_INVALID") {
      error_code = 400;
      error_message = "user is an administrator of the chat";
    } else if (error_message == "File generation failed") {
      error_code = 400;
      error_message = "can't upload file by URL";
    } else if (error_message == "CHAT_ABOUT_NOT_MODIFIED") {
      error_code = 400;
      error_message = "chat description is not modified";
    } else if (error_message == "PACK_SHORT_NAME_INVALID") {
      error_code = 400;
      error_message = "invalid sticker set name is specified";
    } else if (error_message == "PACK_SHORT_NAME_OCCUPIED") {
      error_code = 400;
      error_message = "sticker set name is already occupied";
    } else if (error_message == "STICKER_EMOJI_INVALID") {
      error_code = 400;
      error_message = "invalid sticker emojis";
    } else if (error_message == "QUERY_ID_INVALID") {
      error_code = 400;
      error_message = "query is too old and response timeout expired or query ID is invalid";
    } else if (error_message == "MESSAGE_DELETE_FORBIDDEN") {
      error_code = 400;
      error_message = "message can't be deleted";
    } else if (error_message == "RANK_CHANGE_FORBIDDEN" || error_message == "RANK_EMOJI_NOT_ALLOWED" ||
               error_message == "RANK_INVALID") {
      error_code = 400;
      if (query->method() == "setchatmembertag") {
        error_buf = PSTRING() << "TAG" << error_message.substr(4);
      } else {
        error_buf = PSTRING() << "CUSTOM_TITLE" << error_message.substr(4);
      }
      error_message = error_buf;
    }
  }
  td::Slice prefix;
  switch (error_code) {
    case 400:
      prefix = "Bad Request";
      break;
    case 401:
      prefix = "Unauthorized";
      break;
    case 403:
      prefix = "Forbidden";
      break;
    case 500:
      prefix = "Internal Server Error";
      if (real_error_message != "Request aborted") {
        LOG(ERROR) << "Receive Internal Server Error \"" << real_error_message << "\" from " << *query;
      }
      break;
    default:
      if (!(error_code == 406 && error_message == "FROZEN_METHOD_INVALID")) {
        LOG(ERROR) << "Unsupported error " << real_error_code << ": " << real_error_message << " from " << *query;
      }
      return fail_query(400, PSLICE() << "Bad Request: " << error_message, std::move(query));
  }

  if (td::begins_with(error_message, prefix)) {
    return fail_query(error_code, error_message, std::move(query));
  } else {
    td::string error_str = prefix.str();
    if (error_message.empty()) {
      LOG(ERROR) << "Empty error message with code " << real_error_code << " from " << *query;
    } else {
      error_str += ": ";
      if (error_message.size() >= 2u &&
          (error_message[1] == '_' || ('A' <= error_message[1] && error_message[1] <= 'Z'))) {
        error_str += error_message.str();
      } else {
        error_str += td::to_lower(error_message[0]);
        error_str += error_message.substr(1).str();
      }
    }
    return fail_query(error_code, error_str, std::move(query));
  }
}

void Client::fail_query_with_error(PromisedQueryPtr &&query, object_ptr<td_api::error> error,
                                   td::CSlice default_message) {
  fail_query_with_error(std::move(query), error->code_, error->message_, default_message);
}

bool Client::is_special_error_code(int32 error_code) {
  return error_code == 401 || error_code == 429 || error_code >= 500;
}

bool Client::init_methods() {
  methods_.emplace("getme", &Client::process_get_me_query);
  methods_.emplace("getmycommands", &Client::process_get_my_commands_query);
  methods_.emplace("setmycommands", &Client::process_set_my_commands_query);
  methods_.emplace("deletemycommands", &Client::process_delete_my_commands_query);
  methods_.emplace("getmydefaultadministratorrights", &Client::process_get_my_default_administrator_rights_query);
  methods_.emplace("setmydefaultadministratorrights", &Client::process_set_my_default_administrator_rights_query);
  methods_.emplace("getmyname", &Client::process_get_my_name_query);
  methods_.emplace("setmyname", &Client::process_set_my_name_query);
  methods_.emplace("setmyprofilephoto", &Client::process_set_my_profile_photo_query);
  methods_.emplace("removemyprofilephoto", &Client::process_remove_my_profile_photo_query);
  methods_.emplace("getmydescription", &Client::process_get_my_description_query);
  methods_.emplace("setmydescription", &Client::process_set_my_description_query);
  methods_.emplace("getmyshortdescription", &Client::process_get_my_short_description_query);
  methods_.emplace("setmyshortdescription", &Client::process_set_my_short_description_query);
  methods_.emplace("getchatmenubutton", &Client::process_get_chat_menu_button_query);
  methods_.emplace("setchatmenubutton", &Client::process_set_chat_menu_button_query);
  methods_.emplace("getuserprofilephotos", &Client::process_get_user_profile_photos_query);
  methods_.emplace("getuserprofileaudios", &Client::process_get_user_profile_audios_query);
  methods_.emplace("sendmessage", &Client::process_send_message_query);
  methods_.emplace("sendrichmessage", &Client::process_send_rich_message_query);
  methods_.emplace("sendanimation", &Client::process_send_animation_query);
  methods_.emplace("sendaudio", &Client::process_send_audio_query);
  methods_.emplace("senddice", &Client::process_send_dice_query);
  methods_.emplace("senddocument", &Client::process_send_document_query);
  methods_.emplace("sendlivephoto", &Client::process_send_live_photo_query);
  methods_.emplace("sendphoto", &Client::process_send_photo_query);
  methods_.emplace("sendsticker", &Client::process_send_sticker_query);
  methods_.emplace("sendvideo", &Client::process_send_video_query);
  methods_.emplace("sendvideonote", &Client::process_send_video_note_query);
  methods_.emplace("sendvoice", &Client::process_send_voice_query);
  methods_.emplace("sendpaidmedia", &Client::process_send_paid_media_query);
  methods_.emplace("sendgame", &Client::process_send_game_query);
  methods_.emplace("sendinvoice", &Client::process_send_invoice_query);
  methods_.emplace("sendlocation", &Client::process_send_location_query);
  methods_.emplace("sendvenue", &Client::process_send_venue_query);
  methods_.emplace("sendcontact", &Client::process_send_contact_query);
  methods_.emplace("sendpoll", &Client::process_send_poll_query);
  methods_.emplace("stoppoll", &Client::process_stop_poll_query);
  methods_.emplace("sendchecklist", &Client::process_send_checklist_query);
  methods_.emplace("copymessage", &Client::process_copy_message_query);
  methods_.emplace("copymessages", &Client::process_copy_messages_query);
  methods_.emplace("forwardmessage", &Client::process_forward_message_query);
  methods_.emplace("forwardmessages", &Client::process_forward_messages_query);
  methods_.emplace("sendmediagroup", &Client::process_send_media_group_query);
  methods_.emplace("sendchataction", &Client::process_send_chat_action_query);
  methods_.emplace("sendmessagedraft", &Client::process_send_message_draft_query);
  methods_.emplace("sendrichmessagedraft", &Client::process_send_rich_message_draft_query);
  methods_.emplace("setmessagereaction", &Client::process_set_message_reaction_query);
  methods_.emplace("editmessagetext", &Client::process_edit_message_text_query);
  methods_.emplace("editmessagelivelocation", &Client::process_edit_message_live_location_query);
  methods_.emplace("stopmessagelivelocation", &Client::process_edit_message_live_location_query);
  methods_.emplace("editmessagemedia", &Client::process_edit_message_media_query);
  methods_.emplace("editmessagecaption", &Client::process_edit_message_caption_query);
  methods_.emplace("editmessagechecklist", &Client::process_edit_message_checklist_query);
  methods_.emplace("editmessagereplymarkup", &Client::process_edit_message_reply_markup_query);
  methods_.emplace("editephemeralmessagetext", &Client::process_edit_ephemeral_message_text_query);
  methods_.emplace("editephemeralmessagemedia", &Client::process_edit_ephemeral_message_media_query);
  methods_.emplace("editephemeralmessagecaption", &Client::process_edit_ephemeral_message_caption_query);
  methods_.emplace("editephemeralmessagereplymarkup", &Client::process_edit_ephemeral_message_reply_markup_query);
  methods_.emplace("deletemessage", &Client::process_delete_message_query);
  methods_.emplace("deletemessages", &Client::process_delete_messages_query);
  methods_.emplace("deleteephemeralmessage", &Client::process_delete_ephemeral_message_query);
  methods_.emplace("deletemessagereaction", &Client::process_delete_message_reaction_query);
  methods_.emplace("deleteallmessagereactions", &Client::process_delete_all_message_reactions_query);
  methods_.emplace("poststory", &Client::process_post_story_query);
  methods_.emplace("repoststory", &Client::process_repost_story_query);
  methods_.emplace("editstory", &Client::process_edit_story_query);
  methods_.emplace("deletestory", &Client::process_delete_story_query);
  methods_.emplace("createinvoicelink", &Client::process_create_invoice_link_query);
  methods_.emplace("getmystarbalance", &Client::process_get_my_star_balance_query);
  methods_.emplace("getstartransactions", &Client::process_get_star_transactions_query);
  methods_.emplace("refundstarpayment", &Client::process_refund_star_payment_query);
  methods_.emplace("edituserstarsubscription", &Client::process_edit_user_star_subscription_query);
  methods_.emplace("getavailablegifts", &Client::process_get_available_gifts_query);
  methods_.emplace("sendgift", &Client::process_send_gift_query);
  methods_.emplace("giftpremiumsubscription", &Client::process_gift_premium_subscription_query);
  methods_.emplace("getmanagedbottoken", &Client::process_get_managed_bot_token_query);
  methods_.emplace("getmanagedbotaccesssettings", &Client::process_get_managed_bot_access_settings_query);
  methods_.emplace("setmanagedbotaccesssettings", &Client::process_set_managed_bot_access_settings_query);
  methods_.emplace("replacemanagedbottoken", &Client::process_replace_managed_bot_token_query);
  methods_.emplace("verifyuser", &Client::process_verify_user_query);
  methods_.emplace("verifychat", &Client::process_verify_chat_query);
  methods_.emplace("removeuserverification", &Client::process_remove_user_verification_query);
  methods_.emplace("removechatverification", &Client::process_remove_chat_verification_query);
  methods_.emplace("setgamescore", &Client::process_set_game_score_query);
  methods_.emplace("getgamehighscores", &Client::process_get_game_high_scores_query);
  methods_.emplace("answerwebappquery", &Client::process_answer_web_app_query_query);
  methods_.emplace("answerguestquery", &Client::process_answer_guest_query_query);
  methods_.emplace("answerinlinequery", &Client::process_answer_inline_query_query);
  methods_.emplace("answerchatjoinrequestquery", &Client::process_answer_chat_join_request_query_query);
  methods_.emplace("sendchatjoinrequestwebapp", &Client::process_send_chat_join_request_web_app_query);
  methods_.emplace("savepreparedinlinemessage", &Client::process_save_prepared_inline_message_query);
  methods_.emplace("savepreparedkeyboardbutton", &Client::process_save_prepared_keyboard_button_query);
  methods_.emplace("answercallbackquery", &Client::process_answer_callback_query_query);
  methods_.emplace("answershippingquery", &Client::process_answer_shipping_query_query);
  methods_.emplace("answerprecheckoutquery", &Client::process_answer_pre_checkout_query_query);
  methods_.emplace("exportchatinvitelink", &Client::process_export_chat_invite_link_query);
  methods_.emplace("createchatinvitelink", &Client::process_create_chat_invite_link_query);
  methods_.emplace("createchatsubscriptioninvitelink", &Client::process_create_chat_subscription_invite_link_query);
  methods_.emplace("editchatinvitelink", &Client::process_edit_chat_invite_link_query);
  methods_.emplace("editchatsubscriptioninvitelink", &Client::process_edit_chat_subscription_invite_link_query);
  methods_.emplace("revokechatinvitelink", &Client::process_revoke_chat_invite_link_query);
  methods_.emplace("getbusinessconnection", &Client::process_get_business_connection_query);
  methods_.emplace("readbusinessmessage", &Client::process_read_business_message_query);
  methods_.emplace("deletebusinessmessages", &Client::process_delete_business_messages_query);
  methods_.emplace("setbusinessaccountname", &Client::process_set_business_account_name_query);
  methods_.emplace("setbusinessaccountusername", &Client::process_set_business_account_username_query);
  methods_.emplace("setbusinessaccountbio", &Client::process_set_business_account_bio_query);
  methods_.emplace("setbusinessaccountprofilephoto", &Client::process_set_business_account_profile_photo_query);
  methods_.emplace("removebusinessaccountprofilephoto", &Client::process_remove_business_account_profile_photo_query);
  methods_.emplace("setbusinessaccountgiftsettings", &Client::process_set_business_account_gift_settings_query);
  methods_.emplace("getbusinessaccountstarbalance", &Client::process_get_business_account_star_balance_query);
  methods_.emplace("transferbusinessaccountstars", &Client::process_transfer_business_account_stars_query);
  methods_.emplace("getbusinessaccountgifts", &Client::process_get_business_account_gifts_query);
  methods_.emplace("getusergifts", &Client::process_get_user_gifts_query);
  methods_.emplace("getchatgifts", &Client::process_get_chat_gifts_query);
  methods_.emplace("convertgifttostars", &Client::process_convert_gift_to_stars_query);
  methods_.emplace("upgradegift", &Client::process_upgrade_gift_query);
  methods_.emplace("transfergift", &Client::process_transfer_gift_query);
  methods_.emplace("setuseremojistatus", &Client::process_set_user_emoji_status_query);
  methods_.emplace("getchat", &Client::process_get_chat_query);
  methods_.emplace("getuserpersonalchatmessages", &Client::process_get_user_personal_chat_messages_query);
  methods_.emplace("setchatphoto", &Client::process_set_chat_photo_query);
  methods_.emplace("deletechatphoto", &Client::process_delete_chat_photo_query);
  methods_.emplace("setchattitle", &Client::process_set_chat_title_query);
  methods_.emplace("setchatpermissions", &Client::process_set_chat_permissions_query);
  methods_.emplace("setchatdescription", &Client::process_set_chat_description_query);
  methods_.emplace("pinchatmessage", &Client::process_pin_chat_message_query);
  methods_.emplace("unpinchatmessage", &Client::process_unpin_chat_message_query);
  methods_.emplace("unpinallchatmessages", &Client::process_unpin_all_chat_messages_query);
  methods_.emplace("approvesuggestedpost", &Client::process_approve_suggested_post_query);
  methods_.emplace("declinesuggestedpost", &Client::process_decline_suggested_post_query);
  methods_.emplace("setchatstickerset", &Client::process_set_chat_sticker_set_query);
  methods_.emplace("deletechatstickerset", &Client::process_delete_chat_sticker_set_query);
  methods_.emplace("getforumtopiciconstickers", &Client::process_get_forum_topic_icon_stickers_query);
  methods_.emplace("createforumtopic", &Client::process_create_forum_topic_query);
  methods_.emplace("editforumtopic", &Client::process_edit_forum_topic_query);
  methods_.emplace("closeforumtopic", &Client::process_close_forum_topic_query);
  methods_.emplace("reopenforumtopic", &Client::process_reopen_forum_topic_query);
  methods_.emplace("deleteforumtopic", &Client::process_delete_forum_topic_query);
  methods_.emplace("unpinallforumtopicmessages", &Client::process_unpin_all_forum_topic_messages_query);
  methods_.emplace("editgeneralforumtopic", &Client::process_edit_general_forum_topic_query);
  methods_.emplace("closegeneralforumtopic", &Client::process_close_general_forum_topic_query);
  methods_.emplace("reopengeneralforumtopic", &Client::process_reopen_general_forum_topic_query);
  methods_.emplace("hidegeneralforumtopic", &Client::process_hide_general_forum_topic_query);
  methods_.emplace("unhidegeneralforumtopic", &Client::process_unhide_general_forum_topic_query);
  methods_.emplace("unpinallgeneralforumtopicmessages", &Client::process_unpin_all_general_forum_topic_messages_query);
  methods_.emplace("getchatmember", &Client::process_get_chat_member_query);
  methods_.emplace("getchatadministrators", &Client::process_get_chat_administrators_query);
  methods_.emplace("getchatmembercount", &Client::process_get_chat_member_count_query);
  methods_.emplace("getchatmemberscount", &Client::process_get_chat_member_count_query);
  methods_.emplace("leavechat", &Client::process_leave_chat_query);
  methods_.emplace("promotechatmember", &Client::process_promote_chat_member_query);
  methods_.emplace("setchatadministratorcustomtitle", &Client::process_set_chat_administrator_custom_title_query);
  methods_.emplace("setchatmembertag", &Client::process_set_chat_member_tag_query);
  methods_.emplace("banchatmember", &Client::process_ban_chat_member_query);
  methods_.emplace("kickchatmember", &Client::process_ban_chat_member_query);
  methods_.emplace("restrictchatmember", &Client::process_restrict_chat_member_query);
  methods_.emplace("unbanchatmember", &Client::process_unban_chat_member_query);
  methods_.emplace("banchatsenderchat", &Client::process_ban_chat_sender_chat_query);
  methods_.emplace("unbanchatsenderchat", &Client::process_unban_chat_sender_chat_query);
  methods_.emplace("approvechatjoinrequest", &Client::process_approve_chat_join_request_query);
  methods_.emplace("declinechatjoinrequest", &Client::process_decline_chat_join_request_query);
  methods_.emplace("getuserchatboosts", &Client::process_get_user_chat_boosts_query);
  methods_.emplace("getstickerset", &Client::process_get_sticker_set_query);
  methods_.emplace("getcustomemojistickers", &Client::process_get_custom_emoji_stickers_query);
  methods_.emplace("uploadstickerfile", &Client::process_upload_sticker_file_query);
  methods_.emplace("createnewstickerset", &Client::process_create_new_sticker_set_query);
  methods_.emplace("addstickertoset", &Client::process_add_sticker_to_set_query);
  methods_.emplace("replacestickerinset", &Client::process_replace_sticker_in_set_query);
  methods_.emplace("setstickersettitle", &Client::process_set_sticker_set_title_query);
  methods_.emplace("setstickersetthumb", &Client::process_set_sticker_set_thumbnail_query);
  methods_.emplace("setstickersetthumbnail", &Client::process_set_sticker_set_thumbnail_query);
  methods_.emplace("setcustomemojistickersetthumbnail", &Client::process_set_custom_emoji_sticker_set_thumbnail_query);
  methods_.emplace("deletestickerset", &Client::process_delete_sticker_set_query);
  methods_.emplace("setstickerpositioninset", &Client::process_set_sticker_position_in_set_query);
  methods_.emplace("deletestickerfromset", &Client::process_delete_sticker_from_set_query);
  methods_.emplace("setstickeremojilist", &Client::process_set_sticker_emoji_list_query);
  methods_.emplace("setstickerkeywords", &Client::process_set_sticker_keywords_query);
  methods_.emplace("setstickermaskposition", &Client::process_set_sticker_mask_position_query);
  methods_.emplace("setpassportdataerrors", &Client::process_set_passport_data_errors_query);
  methods_.emplace("sendcustomrequest", &Client::process_send_custom_request_query);
  methods_.emplace("answercustomquery", &Client::process_answer_custom_query_query);
  methods_.emplace("getupdates", &Client::process_get_updates_query);
  methods_.emplace("setwebhook", &Client::process_set_webhook_query);
  methods_.emplace("deletewebhook", &Client::process_set_webhook_query);
  methods_.emplace("getwebhookinfo", &Client::process_get_webhook_info_query);
  methods_.emplace("getfile", &Client::process_get_file_query);
  return true;
}

bool Client::is_local_method(td::Slice method) {
  return method == "close" || method == "logout" || method == "getme" || method == "getupdates" ||
         method == "setwebhook" || method == "deletewebhook" || method == "getwebhookinfo";
}

class Client::JsonEmptyObject final : public td::Jsonable {
 public:
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
  }
};

class Client::JsonFile final : public td::Jsonable {
 public:
  JsonFile(const td_api::file *file, const Client *client, bool with_path)
      : file_(file), client_(client), with_path_(with_path) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    client_->json_store_file(object, file_, with_path_);
  }

 private:
  const td_api::file *file_;
  const Client *client_;
  bool with_path_;
};

class Client::JsonDatedFile final : public td::Jsonable {
 public:
  JsonDatedFile(const td_api::datedFile *file, const Client *client) : file_(file), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    client_->json_store_file(object, file_->file_.get());
    object("file_date", file_->date_);
  }

 private:
  const td_api::datedFile *file_;
  const Client *client_;
};

class Client::JsonDatedFiles final : public td::Jsonable {
 public:
  JsonDatedFiles(const td::vector<object_ptr<td_api::datedFile>> &files, const Client *client)
      : files_(files), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &file : files_) {
      array << JsonDatedFile(file.get(), client_);
    }
  }

 private:
  const td::vector<object_ptr<td_api::datedFile>> &files_;
  const Client *client_;
};

class Client::JsonCommunity final : public td::Jsonable {
 public:
  JsonCommunity(int64 community_id, const Client *client) : community_id_(community_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    auto community_info = client_->get_community_info(community_id_);
    CHECK(community_info != nullptr);
    object("id", community_id_);
    object("name", community_info->name);
  }

 private:
  int64 community_id_;
  const Client *client_;
};

class Client::JsonUser final : public td::Jsonable {
 public:
  JsonUser(int64 user_id, const Client *client, bool full_bot_info = false)
      : user_id_(user_id), client_(client), full_bot_info_(full_bot_info) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    auto user_info = client_->get_user_info(user_id_);
    object("id", user_id_);
    bool is_bot = user_info != nullptr && user_info->type == UserInfo::Type::Bot;
    object("is_bot", td::JsonBool(is_bot));
    object("first_name", user_info == nullptr ? "" : user_info->first_name);
    if (user_info != nullptr && !user_info->last_name.empty()) {
      object("last_name", user_info->last_name);
    }
    if (user_info != nullptr && !user_info->active_usernames.empty()) {
      object("username", user_info->active_usernames[0]);
    }
    if (user_info != nullptr && !user_info->language_code.empty()) {
      object("language_code", user_info->language_code);
    }
    if (user_info != nullptr && user_info->is_premium) {
      object("is_premium", td::JsonTrue());
    }
    if (user_info != nullptr && user_info->added_to_attachment_menu) {
      object("added_to_attachment_menu", td::JsonTrue());
    }
    if (is_bot && full_bot_info_) {
      object("can_join_groups", td::JsonBool(user_info->can_join_groups));
      object("can_read_all_group_messages", td::JsonBool(user_info->can_read_all_group_messages));
      object("supports_inline_queries", td::JsonBool(user_info->is_inline_bot));
      object("supports_guest_queries", td::JsonBool(user_info->supports_guest_queries));
      object("can_connect_to_business", td::JsonBool(user_info->can_connect_to_business));
      object("has_main_web_app", td::JsonBool(user_info->has_main_web_app));
      object("has_topics_enabled", td::JsonBool(user_info->has_topics));
      object("allows_users_to_create_topics", td::JsonBool(user_info->allows_users_to_create_topics));
      object("can_manage_bots", td::JsonBool(user_info->can_manage_bots));
      object("supports_join_request_queries", td::JsonBool(user_info->is_guard));
    }
  }

 private:
  int64 user_id_;
  const Client *client_;
  bool full_bot_info_;
};

class Client::JsonUsers final : public td::Jsonable {
 public:
  JsonUsers(const td::vector<int64> &user_ids, const Client *client) : user_ids_(user_ids), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &user_id : user_ids_) {
      array << JsonUser(user_id, client_);
    }
  }

 private:
  const td::vector<int64> &user_ids_;
  const Client *client_;
};

class Client::JsonEntity final : public td::Jsonable {
 public:
  JsonEntity(const td_api::textEntity *entity, const Client *client) : entity_(entity), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("offset", entity_->offset_);
    object("length", entity_->length_);
    switch (entity_->type_->get_id()) {
      case td_api::textEntityTypeMention::ID:
        object("type", "mention");
        break;
      case td_api::textEntityTypeHashtag::ID:
        object("type", "hashtag");
        break;
      case td_api::textEntityTypeCashtag::ID:
        object("type", "cashtag");
        break;
      case td_api::textEntityTypeBotCommand::ID:
        object("type", "bot_command");
        break;
      case td_api::textEntityTypeUrl::ID:
        object("type", "url");
        break;
      case td_api::textEntityTypeEmailAddress::ID:
        object("type", "email");
        break;
      case td_api::textEntityTypePhoneNumber::ID:
        object("type", "phone_number");
        break;
      case td_api::textEntityTypeBankCardNumber::ID:
        object("type", "bank_card_number");
        break;
      case td_api::textEntityTypeBold::ID:
        object("type", "bold");
        break;
      case td_api::textEntityTypeItalic::ID:
        object("type", "italic");
        break;
      case td_api::textEntityTypeUnderline::ID:
        object("type", "underline");
        break;
      case td_api::textEntityTypeStrikethrough::ID:
        object("type", "strikethrough");
        break;
      case td_api::textEntityTypeSpoiler::ID:
        object("type", "spoiler");
        break;
      case td_api::textEntityTypeCode::ID:
        object("type", "code");
        break;
      case td_api::textEntityTypePre::ID:
        object("type", "pre");
        break;
      case td_api::textEntityTypePreCode::ID: {
        auto entity = static_cast<const td_api::textEntityTypePreCode *>(entity_->type_.get());
        object("type", "pre");
        object("language", entity->language_);
        break;
      }
      case td_api::textEntityTypeTextUrl::ID: {
        auto entity = static_cast<const td_api::textEntityTypeTextUrl *>(entity_->type_.get());
        object("type", "text_link");
        object("url", entity->url_);
        break;
      }
      case td_api::textEntityTypeMentionName::ID: {
        auto entity = static_cast<const td_api::textEntityTypeMentionName *>(entity_->type_.get());
        object("type", "text_mention");
        object("user", JsonUser(entity->user_id_, client_));
        break;
      }
      case td_api::textEntityTypeCustomEmoji::ID: {
        auto entity = static_cast<const td_api::textEntityTypeCustomEmoji *>(entity_->type_.get());
        object("type", "custom_emoji");
        object("custom_emoji_id", td::to_string(entity->custom_emoji_id_));
        break;
      }
      case td_api::textEntityTypeBlockQuote::ID:
        object("type", "blockquote");
        break;
      case td_api::textEntityTypeExpandableBlockQuote::ID:
        object("type", "expandable_blockquote");
        break;
      case td_api::textEntityTypeDateTime::ID: {
        auto entity = static_cast<const td_api::textEntityTypeDateTime *>(entity_->type_.get());
        object("type", "date_time");
        object("unix_time", entity->unix_time_);
        object("date_time_format", get_date_time_format(entity->formatting_type_));
        break;
      }
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::textEntity *entity_;
  const Client *client_;
};

class Client::JsonVectorEntities final : public td::Jsonable {
 public:
  JsonVectorEntities(const td::vector<object_ptr<td_api::textEntity>> &entities, const Client *client)
      : entities_(entities), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &entity : entities_) {
      auto entity_type = entity->type_->get_id();
      if (entity_type != td_api::textEntityTypeBankCardNumber::ID &&
          entity_type != td_api::textEntityTypeMediaTimestamp::ID) {
        array << JsonEntity(entity.get(), client_);
      }
    }
  }

 private:
  const td::vector<object_ptr<td_api::textEntity>> &entities_;
  const Client *client_;
};

class Client::JsonRichMessageButton final : public td::Jsonable {
 public:
  JsonRichMessageButton(const td_api::inlineButton *button, const Client *client) : button_(button), client_(client) {
  }
  void store(td::JsonValueScope *scope) const;

 private:
  const td_api::inlineButton *button_;
  const Client *client_;
};

class Client::JsonRichBlock final : public td::Jsonable {
 public:
  JsonRichBlock(const td_api::PageBlock *block, const Client *client) : block_(block), client_(client) {
  }
  void store(td::JsonValueScope *scope) const;

 private:
  const td_api::PageBlock *block_;
  const Client *client_;
};

class Client::JsonRichBlocks final : public td::Jsonable {
 public:
  JsonRichBlocks(const td::vector<object_ptr<td_api::PageBlock>> &blocks, const Client *client)
      : blocks_(blocks), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (const auto &block : blocks_) {
      array << JsonRichBlock(block.get(), client_);
    }
  }

 private:
  const td::vector<object_ptr<td_api::PageBlock>> &blocks_;
  const Client *client_;
};

class Client::JsonRichText final : public td::Jsonable {
 public:
  JsonRichText(const td_api::RichText *text, const Client *client) : text_(text), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto text_id = text_->get_id();
    if (text_id == td_api::richTextPlain::ID) {
      const auto *text = static_cast<const td_api::richTextPlain *>(text_);
      *scope << td::JsonString(text->text_);
      return;
    }
    if (text_id == td_api::richTextIcon::ID || text_id == td_api::richTextDiff::ID) {
      LOG(ERROR) << "Receive " << to_string(*text_);
      *scope << td::JsonString(td::Slice());
      return;
    }
    if (text_id == td_api::richTexts::ID) {
      auto array = scope->enter_array();
      for (const auto &text : static_cast<const td_api::richTexts *>(text_)->texts_) {
        array << JsonRichText(text.get(), client_);
      }
      return;
    }
    auto object = scope->enter_object();
    switch (text_id) {
      case td_api::richTextBold::ID: {
        const auto *text = static_cast<const td_api::richTextBold *>(text_);
        object("type", "bold");
        object("text", JsonRichText(text->text_.get(), client_));
        break;
      }
      case td_api::richTextItalic::ID: {
        const auto *text = static_cast<const td_api::richTextItalic *>(text_);
        object("type", "italic");
        object("text", JsonRichText(text->text_.get(), client_));
        break;
      }
      case td_api::richTextUnderline::ID: {
        const auto *text = static_cast<const td_api::richTextUnderline *>(text_);
        object("type", "underline");
        object("text", JsonRichText(text->text_.get(), client_));
        break;
      }
      case td_api::richTextStrikethrough::ID: {
        const auto *text = static_cast<const td_api::richTextStrikethrough *>(text_);
        object("type", "strikethrough");
        object("text", JsonRichText(text->text_.get(), client_));
        break;
      }
      case td_api::richTextSpoiler::ID: {
        const auto *text = static_cast<const td_api::richTextSpoiler *>(text_);
        object("type", "spoiler");
        object("text", JsonRichText(text->text_.get(), client_));
        break;
      }
      case td_api::richTextDateTime::ID: {
        const auto *text = static_cast<const td_api::richTextDateTime *>(text_);
        object("type", "date_time");
        object("text", JsonRichText(text->text_.get(), client_));
        object("unix_time", text->unix_time_);
        object("date_time_format", get_date_time_format(text->formatting_type_));
        break;
      }
      case td_api::richTextMention::ID: {
        const auto *text = static_cast<const td_api::richTextMention *>(text_);
        object("type", "mention");
        object("text", JsonRichText(text->text_.get(), client_));
        object("username", text->username_);
        break;
      }
      case td_api::richTextHashtag::ID: {
        const auto *text = static_cast<const td_api::richTextHashtag *>(text_);
        object("type", "hashtag");
        object("text", JsonRichText(text->text_.get(), client_));
        object("hashtag", text->hashtag_);
        break;
      }
      case td_api::richTextCashtag::ID: {
        const auto *text = static_cast<const td_api::richTextCashtag *>(text_);
        object("type", "cashtag");
        object("text", JsonRichText(text->text_.get(), client_));
        object("cashtag", text->cashtag_);
        break;
      }
      case td_api::richTextBotCommand::ID: {
        const auto *text = static_cast<const td_api::richTextBotCommand *>(text_);
        object("type", "bot_command");
        object("text", JsonRichText(text->text_.get(), client_));
        object("bot_command", text->bot_command_);
        break;
      }
      case td_api::richTextFixed::ID: {
        const auto *text = static_cast<const td_api::richTextFixed *>(text_);
        object("type", "code");
        object("text", JsonRichText(text->text_.get(), client_));
        break;
      }
      case td_api::richTextMentionName::ID: {
        const auto *text = static_cast<const td_api::richTextMentionName *>(text_);
        object("type", "text_mention");
        object("text", JsonRichText(text->text_.get(), client_));
        object("user", JsonUser(text->user_id_, client_));
        break;
      }
      case td_api::richTextUrl::ID: {
        const auto *text = static_cast<const td_api::richTextUrl *>(text_);
        object("type", "url");
        object("text", JsonRichText(text->text_.get(), client_));
        object("url", text->url_);
        break;
      }
      case td_api::richTextEmailAddress::ID: {
        const auto *text = static_cast<const td_api::richTextEmailAddress *>(text_);
        object("type", "email_address");
        object("text", JsonRichText(text->text_.get(), client_));
        object("email_address", text->email_address_);
        break;
      }
      case td_api::richTextBankCardNumber::ID: {
        const auto *text = static_cast<const td_api::richTextBankCardNumber *>(text_);
        object("type", "bank_card_number");
        object("text", JsonRichText(text->text_.get(), client_));
        object("bank_card_number", text->bank_card_number_);
        break;
      }
      case td_api::richTextSubscript::ID: {
        const auto *text = static_cast<const td_api::richTextSubscript *>(text_);
        object("type", "subscript");
        object("text", JsonRichText(text->text_.get(), client_));
        break;
      }
      case td_api::richTextSuperscript::ID: {
        const auto *text = static_cast<const td_api::richTextSuperscript *>(text_);
        object("type", "superscript");
        object("text", JsonRichText(text->text_.get(), client_));
        break;
      }
      case td_api::richTextMarked::ID: {
        const auto *text = static_cast<const td_api::richTextMarked *>(text_);
        object("type", "marked");
        object("text", JsonRichText(text->text_.get(), client_));
        break;
      }
      case td_api::richTextPhoneNumber::ID: {
        const auto *text = static_cast<const td_api::richTextPhoneNumber *>(text_);
        object("type", "phone_number");
        object("text", JsonRichText(text->text_.get(), client_));
        object("phone_number", text->phone_number_);
        break;
      }
      case td_api::richTextCustomEmoji::ID: {
        const auto *text = static_cast<const td_api::richTextCustomEmoji *>(text_);
        object("type", "custom_emoji");
        object("custom_emoji_id", td::to_string(text->custom_emoji_id_));
        object("alternative_text", text->alternative_text_);
        break;
      }
      case td_api::richTextMathematicalExpression::ID: {
        const auto *text = static_cast<const td_api::richTextMathematicalExpression *>(text_);
        object("type", "mathematical_expression");
        object("expression", text->expression_);
        break;
      }
      case td_api::richTextReference::ID: {
        const auto *text = static_cast<const td_api::richTextReference *>(text_);
        object("type", "reference");
        object("text", JsonRichText(text->text_.get(), client_));
        object("name", text->name_);
        break;
      }
      case td_api::richTextReferenceLink::ID: {
        const auto *text = static_cast<const td_api::richTextReferenceLink *>(text_);
        object("type", "reference_link");
        object("text", JsonRichText(text->text_.get(), client_));
        object("reference_name", text->reference_name_);
        break;
      }
      case td_api::richTextAnchor::ID: {
        const auto *text = static_cast<const td_api::richTextAnchor *>(text_);
        object("type", "anchor");
        object("name", text->name_);
        break;
      }
      case td_api::richTextAnchorLink::ID: {
        const auto *text = static_cast<const td_api::richTextAnchorLink *>(text_);
        object("type", "anchor_link");
        object("text", JsonRichText(text->text_.get(), client_));
        object("anchor_name", text->anchor_name_);
        break;
      }
      case td_api::richTextButton::ID: {
        const auto *text = static_cast<const td_api::richTextButton *>(text_);
        object("type", "button");
        object("button", JsonRichMessageButton(text->button_.get(), client_));
        break;
      }
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::RichText *text_;
  const Client *client_;
};

void Client::JsonRichMessageButton::store(td::JsonValueScope *scope) const {
  auto object = scope->enter_object();
  object("text", JsonRichText(button_->text_.get(), client_));
  json_store_style(object, button_->style_.get(), true);
  json_store_inline_keyboard_button_type(object, button_->type_.get(), true);
}

class Client::JsonRichTableCell final : public td::Jsonable {
 public:
  JsonRichTableCell(const td_api::pageBlockTableCell *cell, const Client *client) : cell_(cell), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (cell_->text_ != nullptr) {
      object("text", JsonRichText(cell_->text_.get(), client_));
    }
    if (cell_->is_header_) {
      object("is_header", td::JsonTrue());
    }
    if (cell_->colspan_ > 1) {
      object("colspan", cell_->colspan_);
    }
    if (cell_->rowspan_ > 1) {
      object("rowspan", cell_->rowspan_);
    }
    json_store_horizontal_alignment(object, cell_->align_.get());
    switch (cell_->valign_->get_id()) {
      case td_api::pageBlockVerticalAlignmentTop::ID:
        object("valign", "top");
        break;
      case td_api::pageBlockVerticalAlignmentMiddle::ID:
        object("valign", "middle");
        break;
      case td_api::pageBlockVerticalAlignmentBottom::ID:
        object("valign", "bottom");
        break;
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::pageBlockTableCell *cell_;
  const Client *client_;
};

class Client::JsonRichTableRow final : public td::Jsonable {
 public:
  JsonRichTableRow(const td::vector<object_ptr<td_api::pageBlockTableCell>> &cells, const Client *client)
      : cells_(cells), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (const auto &cell : cells_) {
      array << JsonRichTableCell(cell.get(), client_);
    }
  }

 private:
  const td::vector<object_ptr<td_api::pageBlockTableCell>> &cells_;
  const Client *client_;
};

class Client::JsonRichBlockCaption final : public td::Jsonable {
 public:
  JsonRichBlockCaption(const td_api::pageBlockCaption *caption, const Client *client)
      : caption_(caption), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("text", JsonRichText(caption_->text_.get(), client_));
    if (caption_->credit_ != nullptr) {
      object("credit", JsonRichText(caption_->credit_.get(), client_));
    }
  }

 private:
  const td_api::pageBlockCaption *caption_;
  const Client *client_;
};

class Client::JsonRichBlockListItem final : public td::Jsonable {
 public:
  JsonRichBlockListItem(const td_api::pageBlockListItem *item, const Client *client) : item_(item), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("label", item_->label_);
    object("blocks", JsonRichBlocks(item_->blocks_, client_));
    if (item_->has_checkbox_) {
      object("has_checkbox", td::JsonTrue());
      if (item_->is_checked_) {
        object("is_checked", td::JsonTrue());
      }
    }
    if (!item_->type_.empty()) {
      object("type", item_->type_);
      object("value", item_->value_);
    }
  }

 private:
  const td_api::pageBlockListItem *item_;
  const Client *client_;
};

class Client::JsonRichMessage final : public td::Jsonable {
 public:
  JsonRichMessage(const td_api::richMessage *rich_message, const Client *client)
      : rich_message_(rich_message), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("blocks", JsonRichBlocks(rich_message_->blocks_, client_));
    if (rich_message_->is_rtl_) {
      object("is_rtl", td::JsonTrue());
    }
  }

 private:
  const td_api::richMessage *rich_message_;
  const Client *client_;
};

class Client::JsonMaskPosition final : public td::Jsonable {
 public:
  explicit JsonMaskPosition(const td_api::maskPosition *mask_position) : mask_position_(mask_position) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("point", MASK_POINTS[mask_point_to_index(mask_position_->point_)]);
    object("x_shift", mask_position_->x_shift_);
    object("y_shift", mask_position_->y_shift_);
    object("scale", mask_position_->scale_);
  }

 private:
  const td_api::maskPosition *mask_position_;
};

class Client::JsonSticker final : public td::Jsonable {
 public:
  JsonSticker(const td_api::sticker *sticker, const Client *client) : sticker_(sticker), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("width", sticker_->width_);
    object("height", sticker_->height_);
    if (!sticker_->emoji_.empty()) {
      object("emoji", sticker_->emoji_);
    }
    auto set_name = client_->get_sticker_set_name(sticker_->set_id_);
    if (!set_name.empty()) {
      object("set_name", set_name);
    }

    auto format = sticker_->format_->get_id();
    object("is_animated", td::JsonBool(format == td_api::stickerFormatTgs::ID));
    object("is_video", td::JsonBool(format == td_api::stickerFormatWebm::ID));

    switch (sticker_->full_type_->get_id()) {
      case td_api::stickerFullTypeRegular::ID: {
        auto full_type = static_cast<const td_api::stickerFullTypeRegular *>(sticker_->full_type_.get());
        object("type", get_sticker_type(make_object<td_api::stickerTypeRegular>()));
        if (full_type->premium_animation_ != nullptr) {
          object("premium_animation", JsonFile(full_type->premium_animation_.get(), client_, false));
        }
        break;
      }
      case td_api::stickerFullTypeMask::ID: {
        auto full_type = static_cast<const td_api::stickerFullTypeMask *>(sticker_->full_type_.get());
        object("type", get_sticker_type(make_object<td_api::stickerTypeMask>()));
        if (full_type->mask_position_ != nullptr) {
          object("mask_position", JsonMaskPosition(full_type->mask_position_.get()));
        }
        break;
      }
      case td_api::stickerFullTypeCustomEmoji::ID: {
        auto full_type = static_cast<const td_api::stickerFullTypeCustomEmoji *>(sticker_->full_type_.get());
        object("type", get_sticker_type(make_object<td_api::stickerTypeCustomEmoji>()));
        if (full_type->custom_emoji_id_ != 0) {
          object("custom_emoji_id", td::to_string(full_type->custom_emoji_id_));
        }
        if (full_type->needs_repainting_) {
          object("needs_repainting", td::JsonBool(full_type->needs_repainting_));
        }
        break;
      }
      default:
        UNREACHABLE();
        break;
    }

    client_->json_store_thumbnail(object, sticker_->thumbnail_.get());
    client_->json_store_file(object, sticker_->sticker_.get());
  }

 private:
  const td_api::sticker *sticker_;
  const Client *client_;
};

class Client::JsonStickers final : public td::Jsonable {
 public:
  JsonStickers(const td::vector<object_ptr<td_api::sticker>> &stickers, const Client *client)
      : stickers_(stickers), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &sticker : stickers_) {
      array << JsonSticker(sticker.get(), client_);
    }
  }

 private:
  const td::vector<object_ptr<td_api::sticker>> &stickers_;
  const Client *client_;
};

class Client::JsonLiveLocation final : public td::Jsonable {
 public:
  JsonLiveLocation(const td_api::liveLocation *location, double expires_in)
      : location_(location), expires_in_(expires_in) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("latitude", location_->location_->latitude_);
    object("longitude", location_->location_->longitude_);
    if (expires_in_ > 0.0) {
      object("live_period", location_->live_period_);
      if (location_->heading_ > 0) {
        object("heading", location_->heading_);
      }
      if (location_->proximity_alert_radius_ > 0) {
        object("proximity_alert_radius", location_->proximity_alert_radius_);
      }
    }
    if (location_->location_->horizontal_accuracy_ > 0) {
      object("horizontal_accuracy", location_->location_->horizontal_accuracy_);
    }
  }

 private:
  const td_api::liveLocation *location_;
  double expires_in_;
};

class Client::JsonLocation final : public td::Jsonable {
 public:
  explicit JsonLocation(const td_api::location *location) : location_(location) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("latitude", location_->latitude_);
    object("longitude", location_->longitude_);
    if (location_->horizontal_accuracy_ > 0) {
      object("horizontal_accuracy", location_->horizontal_accuracy_);
    }
  }

 private:
  const td_api::location *location_;
};

class Client::JsonReactionType final : public td::Jsonable {
 public:
  explicit JsonReactionType(const td_api::ReactionType *reaction_type) : reaction_type_(reaction_type) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    CHECK(reaction_type_ != nullptr);
    switch (reaction_type_->get_id()) {
      case td_api::reactionTypeEmoji::ID:
        object("type", "emoji");
        object("emoji", static_cast<const td_api::reactionTypeEmoji *>(reaction_type_)->emoji_);
        break;
      case td_api::reactionTypeCustomEmoji::ID:
        object("type", "custom_emoji");
        object("custom_emoji_id",
               td::to_string(static_cast<const td_api::reactionTypeCustomEmoji *>(reaction_type_)->custom_emoji_id_));
        break;
      case td_api::reactionTypePaid::ID:
        object("type", "paid");
        break;
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::ReactionType *reaction_type_;
};

class Client::JsonReactionCount final : public td::Jsonable {
 public:
  explicit JsonReactionCount(const td_api::messageReaction *message_reaction) : message_reaction_(message_reaction) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("type", JsonReactionType(message_reaction_->type_.get()));
    object("total_count", message_reaction_->total_count_);
  }

 private:
  const td_api::messageReaction *message_reaction_;
};

class Client::JsonAcceptedGiftTypes final : public td::Jsonable {
 public:
  JsonAcceptedGiftTypes(bool unlimited_gifts, bool limited_gifts, bool upgraded_gifts, bool premium_subscription,
                        bool gifts_from_channels)
      : unlimited_gifts_(unlimited_gifts)
      , limited_gifts_(limited_gifts)
      , upgraded_gifts_(upgraded_gifts)
      , premium_subscription_(premium_subscription)
      , gifts_from_channels_(gifts_from_channels) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("unlimited_gifts", td::JsonBool(unlimited_gifts_));
    object("limited_gifts", td::JsonBool(limited_gifts_));
    object("unique_gifts", td::JsonBool(upgraded_gifts_));
    object("premium_subscription", td::JsonBool(premium_subscription_));
    object("gifts_from_channels", td::JsonBool(gifts_from_channels_));
  }

 private:
  bool unlimited_gifts_;
  bool limited_gifts_;
  bool upgraded_gifts_;
  bool premium_subscription_;
  bool gifts_from_channels_;
};

class Client::JsonBotAccessSettings final : public td::Jsonable {
 public:
  JsonBotAccessSettings(const td_api::botAccessSettings *bot_access_settings, const Client *client)
      : bot_access_settings_(bot_access_settings), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("is_access_restricted", td::JsonBool(bot_access_settings_->is_restricted_));
    if (!bot_access_settings_->added_user_ids_.empty()) {
      object("added_users", td::json_array(bot_access_settings_->added_user_ids_,
                                           [client = client_](auto &user_id) { return JsonUser(user_id, client); }));
    }
  }

 private:
  const td_api::botAccessSettings *bot_access_settings_;
  const Client *client_;
};

class Client::JsonBirthdate final : public td::Jsonable {
 public:
  explicit JsonBirthdate(const td_api::birthdate *birthdate) : birthdate_(birthdate) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("day", birthdate_->day_);
    object("month", birthdate_->month_);
    if (birthdate_->year_ != 0) {
      object("year", birthdate_->year_);
    }
  }

 private:
  const td_api::birthdate *birthdate_;
};

class Client::JsonBusinessStartPage final : public td::Jsonable {
 public:
  JsonBusinessStartPage(const td_api::businessStartPage *start_page, const Client *client)
      : start_page_(start_page), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (!start_page_->title_.empty()) {
      object("title", start_page_->title_);
    }
    if (!start_page_->message_.empty()) {
      object("message", start_page_->message_);
    }
    if (start_page_->sticker_ != nullptr) {
      object("sticker", JsonSticker(start_page_->sticker_.get(), client_));
    }
  }

 private:
  const td_api::businessStartPage *start_page_;
  const Client *client_;
};

class Client::JsonBusinessLocation final : public td::Jsonable {
 public:
  explicit JsonBusinessLocation(const td_api::businessLocation *business_location)
      : business_location_(business_location) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (business_location_->location_ != nullptr) {
      object("location", JsonLocation(business_location_->location_.get()));
    }
    object("address", business_location_->address_);
  }

 private:
  const td_api::businessLocation *business_location_;
};

class Client::JsonBusinessOpeningHoursInterval final : public td::Jsonable {
 public:
  explicit JsonBusinessOpeningHoursInterval(const td_api::businessOpeningHoursInterval *opening_hours_interval)
      : opening_hours_interval_(opening_hours_interval) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("opening_minute", opening_hours_interval_->start_minute_);
    object("closing_minute", opening_hours_interval_->end_minute_);
  }

 private:
  const td_api::businessOpeningHoursInterval *opening_hours_interval_;
};

class Client::JsonBusinessOpeningHours final : public td::Jsonable {
 public:
  explicit JsonBusinessOpeningHours(const td_api::businessOpeningHours *opening_hours) : opening_hours_(opening_hours) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("opening_hours", td::json_array(opening_hours_->opening_hours_, [](const auto &opening_hours_interval) {
             return JsonBusinessOpeningHoursInterval(opening_hours_interval.get());
           }));
    object("time_zone_name", opening_hours_->time_zone_id_);
  }

 private:
  const td_api::businessOpeningHours *opening_hours_;
};

class Client::JsonChatPermissions final : public td::Jsonable {
 public:
  explicit JsonChatPermissions(const td_api::chatPermissions *chat_permissions) : chat_permissions_(chat_permissions) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    json_store_permissions(object, chat_permissions_);
  }

 private:
  const td_api::chatPermissions *chat_permissions_;
};

class Client::JsonChatPhotoInfo final : public td::Jsonable {
 public:
  explicit JsonChatPhotoInfo(const td_api::chatPhotoInfo *chat_photo) : chat_photo_(chat_photo) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("small_file_id", chat_photo_->small_->remote_->id_);
    object("small_file_unique_id", chat_photo_->small_->remote_->unique_id_);
    object("big_file_id", chat_photo_->big_->remote_->id_);
    object("big_file_unique_id", chat_photo_->big_->remote_->unique_id_);
  }

 private:
  const td_api::chatPhotoInfo *chat_photo_;
};

class Client::JsonChatLocation final : public td::Jsonable {
 public:
  explicit JsonChatLocation(const td_api::chatLocation *chat_location) : chat_location_(chat_location) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("location", JsonLocation(chat_location_->location_.get()));
    object("address", chat_location_->address_);
  }

 private:
  const td_api::chatLocation *chat_location_;
};

class Client::JsonChatInviteLink final : public td::Jsonable {
 public:
  JsonChatInviteLink(const td_api::chatInviteLink *chat_invite_link, const Client *client)
      : chat_invite_link_(chat_invite_link), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("invite_link", chat_invite_link_->invite_link_);
    if (!chat_invite_link_->name_.empty()) {
      object("name", chat_invite_link_->name_);
    }
    object("creator", JsonUser(chat_invite_link_->creator_user_id_, client_));
    if (chat_invite_link_->expiration_date_ != 0) {
      object("expire_date", chat_invite_link_->expiration_date_);
    }
    if (chat_invite_link_->member_limit_ != 0) {
      object("member_limit", chat_invite_link_->member_limit_);
    }
    if (chat_invite_link_->pending_join_request_count_ != 0) {
      object("pending_join_request_count", chat_invite_link_->pending_join_request_count_);
    }
    if (chat_invite_link_->subscription_pricing_ != nullptr) {
      object("subscription_period", chat_invite_link_->subscription_pricing_->period_);
      object("subscription_price", chat_invite_link_->subscription_pricing_->star_count_);
    }
    object("creates_join_request", td::JsonBool(chat_invite_link_->creates_join_request_));
    object("is_primary", td::JsonBool(chat_invite_link_->is_primary_));
    object("is_revoked", td::JsonBool(chat_invite_link_->is_revoked_));
  }

 private:
  const td_api::chatInviteLink *chat_invite_link_;
  const Client *client_;
};

class Client::JsonUserRating final : public td::Jsonable {
 public:
  explicit JsonUserRating(const td_api::userRating *user_rating) : user_rating_(user_rating) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("level", user_rating_->level_);
    object("rating", user_rating_->rating_);
    object("current_level_rating", user_rating_->current_level_rating_);
    if (!user_rating_->is_maximum_level_reached_) {
      object("next_level_rating", user_rating_->next_level_rating_);
    }
  }

 private:
  const td_api::userRating *user_rating_;
};

class Client::JsonUniqueGiftColors final : public td::Jsonable {
 public:
  explicit JsonUniqueGiftColors(const td_api::upgradedGiftColors *gift_colors) : gift_colors_(gift_colors) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("model_custom_emoji_id", td::to_string(gift_colors_->model_custom_emoji_id_));
    object("symbol_custom_emoji_id", td::to_string(gift_colors_->symbol_custom_emoji_id_));
    object("light_theme_main_color", gift_colors_->light_theme_accent_color_);
    object("light_theme_other_colors",
           td::json_array(gift_colors_->light_theme_colors_, [](int32 color) { return color; }));
    object("dark_theme_main_color", gift_colors_->dark_theme_accent_color_);
    object("dark_theme_other_colors",
           td::json_array(gift_colors_->dark_theme_colors_, [](int32 color) { return color; }));
  }

 private:
  const td_api::upgradedGiftColors *gift_colors_;
};

class Client::JsonAudio final : public td::Jsonable {
 public:
  JsonAudio(const td_api::audio *audio, const Client *client) : audio_(audio), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("duration", audio_->duration_);
    if (!audio_->file_name_.empty()) {
      object("file_name", audio_->file_name_);
    }
    if (!audio_->mime_type_.empty()) {
      object("mime_type", audio_->mime_type_);
    }
    if (!audio_->title_.empty()) {
      object("title", audio_->title_);
    }
    if (!audio_->performer_.empty()) {
      object("performer", audio_->performer_);
    }
    client_->json_store_thumbnail(object, audio_->album_cover_thumbnail_.get());
    client_->json_store_file(object, audio_->audio_.get());
  }

 private:
  const td_api::audio *audio_;
  const Client *client_;
};

class Client::JsonAudios final : public td::Jsonable {
 public:
  JsonAudios(const td_api::audios *audios, const Client *client) : audios_(audios), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("total_count", audios_->total_count_);
    object("audios", td::json_array(audios_->audios_,
                                    [client = client_](auto &audio) { return JsonAudio(audio.get(), client); }));
  }

 private:
  const td_api::audios *audios_;
  const Client *client_;
};

class Client::JsonMessage final : public td::Jsonable {
 public:
  JsonMessage(const MessageInfo *message, bool need_reply, const td::string &source, const Client *client)
      : message_(message), need_reply_(need_reply), source_(source), client_(client) {
  }
  void store(td::JsonValueScope *scope) const;

 private:
  const MessageInfo *message_;
  bool need_reply_;
  const td::string &source_;
  const Client *client_;

  void add_caption(td::JsonObjectScope &object, const object_ptr<td_api::formattedText> &caption,
                   bool show_caption_above_media) const {
    CHECK(caption != nullptr);
    if (!caption->text_.empty()) {
      object("caption", caption->text_);

      if (!caption->entities_.empty()) {
        object("caption_entities", JsonVectorEntities(caption->entities_, client_));
      }

      if (show_caption_above_media) {
        object("show_caption_above_media", td::JsonTrue());
      }
    }
  }

  void add_media_spoiler(td::JsonObjectScope &object, bool has_spoiler) const {
    if (has_spoiler) {
      object("has_media_spoiler", td::JsonTrue());
    }
  }
};

class Client::JsonChat final : public td::Jsonable {
 public:
  JsonChat(int64 chat_id, const Client *client, bool is_full = false, int64 pinned_message_id = -1)
      : chat_id_(chat_id), client_(client), is_full_(is_full), pinned_message_id_(pinned_message_id) {
  }
  void store(td::JsonValueScope *scope) const {
    auto chat_info = client_->get_chat(chat_id_);
    CHECK(chat_info != nullptr);
    auto object = scope->enter_object();
    object("id", chat_id_);
    const td_api::chatPhoto *photo = nullptr;
    switch (chat_info->type) {
      case ChatInfo::Type::Private: {
        auto user_info = client_->get_user_info(chat_info->user_id);
        CHECK(user_info != nullptr);
        object("first_name", user_info->first_name);
        if (!user_info->last_name.empty()) {
          object("last_name", user_info->last_name);
        }
        if (!user_info->active_usernames.empty()) {
          object("username", user_info->active_usernames[0]);
        }
        object("type", "private");
        if (is_full_) {
          if (user_info->type == UserInfo::Type::Regular) {
            object("can_send_gift", td::JsonTrue());
          }
          if (!user_info->active_usernames.empty()) {
            object("active_usernames", td::json_array(user_info->active_usernames,
                                                      [](td::Slice username) { return td::JsonString(username); }));
          }
          if (!user_info->bio.empty()) {
            object("bio", user_info->bio);
          }
          if (user_info->has_private_forwards) {
            object("has_private_forwards", td::JsonTrue());
          }
          if (user_info->has_restricted_voice_and_video_messages) {
            object("has_restricted_voice_and_video_messages", td::JsonTrue());
          }
          if (user_info->business_info != nullptr) {
            auto business_info = user_info->business_info.get();
            if (business_info->start_page_ != nullptr) {
              object("business_intro", JsonBusinessStartPage(business_info->start_page_.get(), client_));
            }
            if (business_info->location_ != nullptr) {
              object("business_location", JsonBusinessLocation(business_info->location_.get()));
            }
            if (business_info->opening_hours_ != nullptr) {
              object("business_opening_hours", JsonBusinessOpeningHours(business_info->opening_hours_.get()));
            }
          }
          CHECK(user_info->accepted_gift_types != nullptr);
          object("accepted_gift_types", JsonAcceptedGiftTypes(user_info->accepted_gift_types->unlimited_gifts_,
                                                              user_info->accepted_gift_types->limited_gifts_,
                                                              user_info->accepted_gift_types->upgraded_gifts_,
                                                              user_info->accepted_gift_types->premium_subscription_,
                                                              user_info->accepted_gift_types->gifts_from_channels_));
          if (user_info->birthdate != nullptr) {
            object("birthdate", JsonBirthdate(user_info->birthdate.get()));
          }
          if (user_info->personal_chat_id != 0) {
            object("personal_chat", JsonChat(user_info->personal_chat_id, client_));
          }
          if (user_info->community_id != 0) {
            object("community", JsonCommunity(user_info->community_id, client_));
          }
          if (user_info->rating != nullptr) {
            object("rating", JsonUserRating(user_info->rating.get()));
          }
          if (user_info->first_profile_audio != nullptr) {
            object("first_profile_audio", JsonAudio(user_info->first_profile_audio.get(), client_));
          }
          if (user_info->paid_message_star_count > 0) {
            object("paid_message_star_count", user_info->paid_message_star_count);
          }
          photo = user_info->photo.get();
        }
        break;
      }
      case ChatInfo::Type::Group: {
        object("title", chat_info->title);
        object("type", "group");
        const auto *permissions = chat_info->permissions.get();
        auto everyone_is_administrator =
            permissions->can_send_basic_messages_ && permissions->can_send_audios_ &&
            permissions->can_send_documents_ && permissions->can_send_photos_ && permissions->can_send_videos_ &&
            permissions->can_send_video_notes_ && permissions->can_send_voice_notes_ && permissions->can_send_polls_ &&
            permissions->can_send_other_messages_ && permissions->can_add_link_previews_ &&
            permissions->can_react_to_messages_ && permissions->can_edit_tag_ && permissions->can_change_info_ &&
            permissions->can_invite_users_ && permissions->can_pin_messages_;
        object("all_members_are_administrators", td::JsonBool(everyone_is_administrator));
        if (is_full_) {
          auto group_info = client_->get_group_info(chat_info->group_id);
          CHECK(group_info != nullptr);
          if (!group_info->description.empty()) {
            object("description", group_info->description);
          }
          if (!group_info->invite_link.empty()) {
            object("invite_link", group_info->invite_link);
          }
          object("permissions", JsonChatPermissions(permissions));
          object("accepted_gift_types", JsonAcceptedGiftTypes(false, false, false, false, false));
          photo = group_info->photo.get();
        }
        break;
      }
      case ChatInfo::Type::Supergroup: {
        object("title", chat_info->title);

        auto supergroup_info = client_->get_supergroup_info(chat_info->supergroup_id);
        CHECK(supergroup_info != nullptr);
        if (!supergroup_info->active_usernames.empty()) {
          object("username", supergroup_info->active_usernames[0]);
        }
        if (supergroup_info->is_supergroup) {
          if (supergroup_info->is_forum) {
            object("is_forum", td::JsonTrue());
          }
          if (supergroup_info->is_direct_messages) {
            object("is_direct_messages", td::JsonTrue());
          }
        }

        if (supergroup_info->is_supergroup) {
          object("type", "supergroup");
        } else {
          object("type", "channel");
        }
        if (is_full_) {
          if (!supergroup_info->active_usernames.empty()) {
            object("active_usernames", td::json_array(supergroup_info->active_usernames,
                                                      [](td::Slice username) { return td::JsonString(username); }));
          }
          if (!supergroup_info->description.empty()) {
            object("description", supergroup_info->description);
          }
          if (!supergroup_info->invite_link.empty()) {
            object("invite_link", supergroup_info->invite_link);
          }
          if (supergroup_info->sticker_set_id != 0) {
            auto sticker_set_name = client_->get_sticker_set_name(supergroup_info->sticker_set_id);
            if (!sticker_set_name.empty()) {
              object("sticker_set_name", sticker_set_name);
            }
          }
          if (supergroup_info->custom_emoji_sticker_set_id != 0) {
            auto sticker_set_name = client_->get_sticker_set_name(supergroup_info->custom_emoji_sticker_set_id);
            if (!sticker_set_name.empty()) {
              object("custom_emoji_sticker_set_name", sticker_set_name);
            }
          }
          if (supergroup_info->can_send_gift) {
            object("can_send_gift", td::JsonTrue());
          }
          if (supergroup_info->can_set_sticker_set) {
            object("can_set_sticker_set", td::JsonTrue());
          }
          if (supergroup_info->is_all_history_available) {
            object("has_visible_history", td::JsonTrue());
          }
          if (supergroup_info->is_supergroup) {
            object("permissions", JsonChatPermissions(chat_info->permissions.get()));
          }
          if (supergroup_info->is_supergroup && supergroup_info->join_to_send_messages) {
            object("join_to_send_messages", td::JsonTrue());
          }
          if (supergroup_info->is_supergroup && supergroup_info->join_by_request) {
            object("join_by_request", td::JsonTrue());
          }
          if (supergroup_info->is_supergroup && supergroup_info->has_hidden_members) {
            object("has_hidden_members", td::JsonTrue());
          }
          if (supergroup_info->has_aggressive_anti_spam_enabled) {
            object("has_aggressive_anti_spam_enabled", td::JsonTrue());
          }
          if (supergroup_info->slow_mode_delay != 0) {
            object("slow_mode_delay", supergroup_info->slow_mode_delay);
          }
          if (supergroup_info->unrestrict_boost_count != 0) {
            object("unrestrict_boost_count", supergroup_info->unrestrict_boost_count);
          }
          if (supergroup_info->linked_chat_id != 0) {
            object("linked_chat_id", supergroup_info->linked_chat_id);
          }
          if (supergroup_info->is_direct_messages && supergroup_info->direct_messages_chat_id != 0) {
            object("parent_chat", JsonChat(supergroup_info->direct_messages_chat_id, client_));
          }
          if (supergroup_info->guard_bot_user_id != 0) {
            object("guard_bot", JsonUser(supergroup_info->guard_bot_user_id, client_));
          }
          if (supergroup_info->community_id != 0) {
            object("community", JsonCommunity(supergroup_info->community_id, client_));
          }
          if (supergroup_info->location != nullptr) {
            object("location", JsonChatLocation(supergroup_info->location.get()));
          }
          if (supergroup_info->has_paid_media_allowed && !supergroup_info->is_supergroup) {
            object("can_send_paid_media", td::JsonTrue());
          }
          object("accepted_gift_types",
                 JsonAcceptedGiftTypes(supergroup_info->can_send_gift, supergroup_info->can_send_gift,
                                       supergroup_info->can_send_gift, supergroup_info->can_send_gift,
                                       supergroup_info->can_send_gift));
          if (supergroup_info->paid_message_star_count) {
            object("paid_message_star_count", supergroup_info->paid_message_star_count);
          }
          photo = supergroup_info->photo.get();
        }
        break;
      }
      case ChatInfo::Type::Unknown:
      default:
        UNREACHABLE();
    }
    if (is_full_) {
      if (photo != nullptr) {
        const td_api::file *small_file = nullptr;
        const td_api::file *big_file = nullptr;
        for (auto &size : photo->sizes_) {
          if (size->type_ == "a") {
            small_file = size->photo_.get();
          } else if (size->type_ == "c") {
            big_file = size->photo_.get();
          }
        }
        if (small_file == nullptr || big_file == nullptr) {
          LOG(ERROR) << "Failed to convert chatPhoto to chatPhotoInfo for " << chat_id_ << ": " << to_string(*photo);
        } else {
          if (chat_info->photo_info == nullptr) {
            LOG(ERROR) << "Have chatPhoto without chatPhotoInfo for " << chat_id_;
          } else {
            if (small_file->remote_->unique_id_ != chat_info->photo_info->small_->remote_->unique_id_ ||
                big_file->remote_->unique_id_ != chat_info->photo_info->big_->remote_->unique_id_) {
              LOG(ERROR) << "Have different chatPhoto and chatPhotoInfo for " << chat_id_ << ": " << to_string(*photo)
                         << ' ' << to_string(chat_info->photo_info);
            }
          }
        }
      } else if (chat_info->photo_info != nullptr) {
        LOG(ERROR) << "Have chatPhotoInfo without chatPhoto for " << chat_id_;
      }
      if (chat_info->photo_info != nullptr) {
        object("photo", JsonChatPhotoInfo(chat_info->photo_info.get()));
      }
      if (pinned_message_id_ != 0) {
        const MessageInfo *pinned_message = client_->get_message(chat_id_, pinned_message_id_, true);
        if (pinned_message != nullptr) {
          object("pinned_message", JsonMessage(pinned_message, false, "pin in JsonChat", client_));
        } else {
          LOG(INFO) << "Pinned unknown, inaccessible or deleted message " << pinned_message_id_;
        }
      }
      if (chat_info->message_auto_delete_time != 0) {
        object("message_auto_delete_time", chat_info->message_auto_delete_time);
      }
      if (chat_info->emoji_status_custom_emoji_id != 0) {
        object("emoji_status_custom_emoji_id", td::to_string(chat_info->emoji_status_custom_emoji_id));
        if (chat_info->emoji_status_expiration_date != 0) {
          object("emoji_status_expiration_date", chat_info->emoji_status_expiration_date);
        }
      }
      if (chat_info->available_reactions != nullptr) {
        object("available_reactions",
               td::json_array(chat_info->available_reactions->reactions_,
                              [](const auto &reaction) { return JsonReactionType(reaction.get()); }));
      }
      object("max_reaction_count", chat_info->max_reaction_count);
      CHECK(chat_info->accent_color_id != -1);
      object("accent_color_id", chat_info->accent_color_id);
      if (chat_info->background_custom_emoji_id != 0) {
        object("background_custom_emoji_id", td::to_string(chat_info->background_custom_emoji_id));
      }
      if (chat_info->upgraded_gift_colors != nullptr) {
        object("unique_gift_colors", JsonUniqueGiftColors(chat_info->upgraded_gift_colors.get()));
      }
      if (chat_info->profile_accent_color_id != -1) {
        object("profile_accent_color_id", chat_info->profile_accent_color_id);
      }
      if (chat_info->profile_background_custom_emoji_id != 0) {
        object("profile_background_custom_emoji_id", td::to_string(chat_info->profile_background_custom_emoji_id));
      }
      if (chat_info->has_protected_content) {
        object("has_protected_content", td::JsonTrue());
      }
    }
  }

 private:
  int64 chat_id_;
  const Client *client_;
  bool is_full_;
  int64 pinned_message_id_;
};

class Client::JsonGiftBackground final : public td::Jsonable {
 public:
  explicit JsonGiftBackground(const td_api::giftBackground *background) : background_(background) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("center_color", background_->center_color_);
    object("edge_color", background_->edge_color_);
    object("text_color", background_->text_color_);
  }

 private:
  const td_api::giftBackground *background_;
};

class Client::JsonGift final : public td::Jsonable {
 public:
  JsonGift(const td_api::gift *gift, const Client *client) : gift_(gift), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(gift_->id_));
    object("sticker", JsonSticker(gift_->sticker_.get(), client_));
    object("star_count", gift_->star_count_);
    if (gift_->upgrade_star_count_ > 0) {
      object("upgrade_star_count", gift_->upgrade_star_count_);
    }
    if (gift_->overall_limits_ != nullptr && gift_->overall_limits_->total_count_ > 0) {
      object("remaining_count", gift_->overall_limits_->remaining_count_);
      object("total_count", gift_->overall_limits_->total_count_);
    }
    if (gift_->user_limits_ != nullptr && gift_->user_limits_->total_count_ > 0) {
      object("personal_remaining_count", gift_->user_limits_->remaining_count_);
      object("personal_total_count", gift_->user_limits_->total_count_);
    }
    if (gift_->publisher_chat_id_ != 0) {
      object("publisher_chat", JsonChat(gift_->publisher_chat_id_, client_));
    }
    if (gift_->is_premium_) {
      object("is_premium", td::JsonTrue());
    }
    if (gift_->has_colors_) {
      object("has_colors", td::JsonTrue());
    }
    if (gift_->upgrade_variant_count_ > 0) {
      object("unique_gift_variant_count", gift_->upgrade_variant_count_);
    }
    if (gift_->background_ != nullptr) {
      object("background", JsonGiftBackground(gift_->background_.get()));
    }
  }

 private:
  const td_api::gift *gift_;
  const Client *client_;
};

class Client::JsonGifts final : public td::Jsonable {
 public:
  JsonGifts(const td::vector<object_ptr<td_api::gift>> &gifts, const Client *client) : gifts_(gifts), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("gifts", td::json_array(gifts_, [client = client_](auto &gift) { return JsonGift(gift.get(), client); }));
  }

 private:
  const td::vector<object_ptr<td_api::gift>> &gifts_;
  const Client *client_;
};

class Client::JsonUniqueGiftModel final : public td::Jsonable {
 public:
  JsonUniqueGiftModel(const td_api::upgradedGiftModel *model, const Client *client) : model_(model), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("name", model_->name_);
    object("sticker", JsonSticker(model_->sticker_.get(), client_));
    json_store_rarity(object, model_->rarity_.get());
  }

 private:
  const td_api::upgradedGiftModel *model_;
  const Client *client_;
};

class Client::JsonUniqueGiftSymbol final : public td::Jsonable {
 public:
  JsonUniqueGiftSymbol(const td_api::upgradedGiftSymbol *symbol, const Client *client)
      : symbol_(symbol), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("name", symbol_->name_);
    object("sticker", JsonSticker(symbol_->sticker_.get(), client_));
    json_store_rarity(object, symbol_->rarity_.get());
  }

 private:
  const td_api::upgradedGiftSymbol *symbol_;
  const Client *client_;
};

class Client::JsonUniqueGiftBackdropColors final : public td::Jsonable {
 public:
  explicit JsonUniqueGiftBackdropColors(const td_api::upgradedGiftBackdropColors *colors) : colors_(colors) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("center_color", colors_->center_color_);
    object("edge_color", colors_->edge_color_);
    object("symbol_color", colors_->symbol_color_);
    object("text_color", colors_->text_color_);
  }

 private:
  const td_api::upgradedGiftBackdropColors *colors_;
};

class Client::JsonUniqueGiftBackdrop final : public td::Jsonable {
 public:
  explicit JsonUniqueGiftBackdrop(const td_api::upgradedGiftBackdrop *backdrop) : backdrop_(backdrop) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("name", backdrop_->name_);
    object("colors", JsonUniqueGiftBackdropColors(backdrop_->colors_.get()));
    json_store_rarity(object, backdrop_->rarity_.get());
  }

 private:
  const td_api::upgradedGiftBackdrop *backdrop_;
};

class Client::JsonUniqueGift final : public td::Jsonable {
 public:
  JsonUniqueGift(const td_api::upgradedGift *gift, const Client *client) : gift_(gift), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("gift_id", td::to_string(gift_->regular_gift_id_));
    object("base_name", gift_->title_);
    object("name", gift_->name_);
    object("number", gift_->number_);
    object("model", JsonUniqueGiftModel(gift_->model_.get(), client_));
    object("symbol", JsonUniqueGiftSymbol(gift_->symbol_.get(), client_));
    object("backdrop", JsonUniqueGiftBackdrop(gift_->backdrop_.get()));
    if (gift_->publisher_chat_id_ != 0) {
      object("publisher_chat", JsonChat(gift_->publisher_chat_id_, client_));
    }
    if (gift_->is_premium_) {
      object("is_premium", td::JsonTrue());
    }
    if (gift_->colors_ != nullptr) {
      object("colors", JsonUniqueGiftColors(gift_->colors_.get()));
    }
    if (gift_->host_id_ != nullptr) {
      object("is_from_blockchain", td::JsonTrue());
    }
    if (gift_->is_burned_) {
      object("is_burned", td::JsonTrue());
    }
  }

 private:
  const td_api::upgradedGift *gift_;
  const Client *client_;
};

class Client::JsonStarAmount final : public td::Jsonable {
 public:
  explicit JsonStarAmount(const td_api::starAmount *amount) : amount_(amount) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("amount", amount_->star_count_);
    if (amount_->nanostar_count_ != 0) {
      object("nanostar_amount", amount_->nanostar_count_);
    }
  }

 private:
  const td_api::starAmount *amount_;
};

class Client::JsonSuggestedPostPrice final : public td::Jsonable {
 public:
  explicit JsonSuggestedPostPrice(const td_api::SuggestedPostPrice *suggested_post_price)
      : suggested_post_price_(suggested_post_price) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    switch (suggested_post_price_->get_id()) {
      case td_api::suggestedPostPriceStar::ID:
        object("currency", "XTR");
        object("amount", static_cast<const td_api::suggestedPostPriceStar *>(suggested_post_price_)->star_count_);
        break;
      case td_api::suggestedPostPriceGram::ID:
        object("currency", "TON");
        object("amount",
               static_cast<const td_api::suggestedPostPriceGram *>(suggested_post_price_)->gram_cent_count_ * 10000000);
        break;
      default:
        UNREACHABLE();
        break;
    }
  }

 private:
  const td_api::SuggestedPostPrice *suggested_post_price_;
};

class Client::JsonSuggestedPostInfo final : public td::Jsonable {
 public:
  explicit JsonSuggestedPostInfo(const td_api::suggestedPostInfo *suggested_post_info)
      : suggested_post_info_(suggested_post_info) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (suggested_post_info_->price_ != nullptr) {
      object("price", JsonSuggestedPostPrice(suggested_post_info_->price_.get()));
    }
    if (suggested_post_info_->send_date_ != 0) {
      object("send_date", suggested_post_info_->send_date_);
    }
    switch (suggested_post_info_->state_->get_id()) {
      case td_api::suggestedPostStatePending::ID:
        object("state", "pending");
        break;
      case td_api::suggestedPostStateApproved::ID:
        object("state", "approved");
        break;
      case td_api::suggestedPostStateDeclined::ID:
        object("state", "declined");
        break;
      default:
        UNREACHABLE();
        break;
    }
  }

 private:
  const td_api::suggestedPostInfo *suggested_post_info_;
};

class Client::JsonInaccessibleMessage final : public td::Jsonable {
 public:
  JsonInaccessibleMessage(int64 chat_id, int64 message_id, const Client *client)
      : chat_id_(chat_id), message_id_(message_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("message_id", as_client_message_id(message_id_));
    object("chat", JsonChat(chat_id_, client_));
    object("date", 0);
  }

 private:
  int64 chat_id_;
  int64 message_id_;
  const Client *client_;
};

class Client::JsonMessageSender final : public td::Jsonable {
 public:
  JsonMessageSender(const td_api::MessageSender *sender_id, const Client *client)
      : sender_id_(sender_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    CHECK(sender_id_ != nullptr);
    switch (sender_id_->get_id()) {
      case td_api::messageSenderUser::ID: {
        auto sender_user_id = static_cast<const td_api::messageSenderUser *>(sender_id_)->user_id_;
        JsonUser(sender_user_id, client_).store(scope);
        break;
      }
      case td_api::messageSenderChat::ID: {
        auto sender_chat_id = static_cast<const td_api::messageSenderChat *>(sender_id_)->chat_id_;
        JsonChat(sender_chat_id, client_).store(scope);
        break;
      }
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::MessageSender *sender_id_;
  const Client *client_;
};

class Client::JsonMessageOrigin final : public td::Jsonable {
 public:
  JsonMessageOrigin(const td_api::MessageOrigin *message_origin, int32 initial_send_date, const Client *client)
      : message_origin_(message_origin), initial_send_date_(initial_send_date), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    switch (message_origin_->get_id()) {
      case td_api::messageOriginUser::ID: {
        auto origin = static_cast<const td_api::messageOriginUser *>(message_origin_);
        object("type", "user");
        object("sender_user", JsonUser(origin->sender_user_id_, client_));
        break;
      }
      case td_api::messageOriginChat::ID: {
        auto origin = static_cast<const td_api::messageOriginChat *>(message_origin_);
        object("type", "chat");
        object("sender_chat", JsonChat(origin->sender_chat_id_, client_));
        if (!origin->author_signature_.empty()) {
          object("author_signature", origin->author_signature_);
        }
        break;
      }
      case td_api::messageOriginHiddenUser::ID: {
        auto origin = static_cast<const td_api::messageOriginHiddenUser *>(message_origin_);
        object("type", "hidden_user");
        if (!origin->sender_name_.empty()) {
          object("sender_user_name", origin->sender_name_);
        }
        break;
      }
      case td_api::messageOriginChannel::ID: {
        auto origin = static_cast<const td_api::messageOriginChannel *>(message_origin_);
        object("type", "channel");
        object("chat", JsonChat(origin->chat_id_, client_));
        object("message_id", as_client_message_id(origin->message_id_));
        if (!origin->author_signature_.empty()) {
          object("author_signature", origin->author_signature_);
        }
        break;
      }
      default:
        UNREACHABLE();
    }
    object("date", initial_send_date_);
  }

 private:
  const td_api::MessageOrigin *message_origin_;
  int32 initial_send_date_;
  const Client *client_;
};

class Client::JsonMessages final : public td::Jsonable {
 public:
  explicit JsonMessages(const td::vector<td::string> &messages) : messages_(messages) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &message : messages_) {
      array << td::JsonRaw(message);
    }
  }

 private:
  const td::vector<td::string> &messages_;
};

class Client::JsonLinkPreviewOptions final : public td::Jsonable {
 public:
  explicit JsonLinkPreviewOptions(const td_api::linkPreviewOptions *link_preview_options)
      : link_preview_options_(link_preview_options) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (link_preview_options_->is_disabled_) {
      object("is_disabled", td::JsonTrue());
    }
    if (!link_preview_options_->url_.empty()) {
      object("url", link_preview_options_->url_);
    }
    if (link_preview_options_->force_small_media_) {
      object("prefer_small_media", td::JsonTrue());
    }
    if (link_preview_options_->force_large_media_) {
      object("prefer_large_media", td::JsonTrue());
    }
    if (link_preview_options_->show_above_text_) {
      object("show_above_text", td::JsonTrue());
    }
  }

 private:
  const td_api::linkPreviewOptions *link_preview_options_;
};

class Client::JsonAnimation final : public td::Jsonable {
 public:
  JsonAnimation(const td_api::animation *animation, bool as_document, const Client *client)
      : animation_(animation), as_document_(as_document), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (!animation_->file_name_.empty()) {
      object("file_name", animation_->file_name_);
    }
    if (!animation_->mime_type_.empty()) {
      object("mime_type", animation_->mime_type_);
    }
    if (!as_document_) {
      object("duration", animation_->duration_);
      object("width", animation_->width_);
      object("height", animation_->height_);
    }
    client_->json_store_thumbnail(object, animation_->thumbnail_.get());
    client_->json_store_file(object, animation_->animation_.get());
  }

 private:
  const td_api::animation *animation_;
  bool as_document_;
  const Client *client_;
};

class Client::JsonDocument final : public td::Jsonable {
 public:
  JsonDocument(const td_api::document *document, const Client *client) : document_(document), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (!document_->file_name_.empty()) {
      object("file_name", document_->file_name_);
    }
    if (!document_->mime_type_.empty()) {
      object("mime_type", document_->mime_type_);
    }
    client_->json_store_thumbnail(object, document_->thumbnail_.get());
    client_->json_store_file(object, document_->document_.get());
  }

 private:
  const td_api::document *document_;
  const Client *client_;
};

class Client::JsonLink final : public td::Jsonable {
 public:
  explicit JsonLink(const td::string &url) : url_(url) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("url", url_);
  }

 private:
  const td::string &url_;
};

class Client::JsonPhotoSize final : public td::Jsonable {
 public:
  JsonPhotoSize(const td_api::photoSize *photo_size, const Client *client) : photo_size_(photo_size), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    client_->json_store_file(object, photo_size_->photo_.get());
    object("width", photo_size_->width_);
    object("height", photo_size_->height_);
  }

 private:
  const td_api::photoSize *photo_size_;
  const Client *client_;
};

class Client::JsonThumbnail final : public td::Jsonable {
 public:
  JsonThumbnail(const td_api::thumbnail *thumbnail, const Client *client) : thumbnail_(thumbnail), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    client_->json_store_file(object, thumbnail_->file_.get());
    object("width", thumbnail_->width_);
    object("height", thumbnail_->height_);
  }

 private:
  const td_api::thumbnail *thumbnail_;
  const Client *client_;
};

class Client::JsonPhoto final : public td::Jsonable {
 public:
  JsonPhoto(const td_api::photo *photo, const Client *client) : photo_(photo), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &photo_size : photo_->sizes_) {
      if (photo_size->type_ != "i" && photo_size->type_ != "t" && !photo_size->photo_->remote_->id_.empty()) {
        array << JsonPhotoSize(photo_size.get(), client_);
      }
    }
  }

 private:
  const td_api::photo *photo_;
  const Client *client_;
};

class Client::JsonLivePhoto final : public td::Jsonable {
 public:
  JsonLivePhoto(const td_api::photo *photo, const td_api::video *video, const Client *client)
      : photo_(photo), video_(video), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("photo", JsonPhoto(photo_, client_));
    object("duration", video_->duration_);
    object("width", video_->width_);
    object("height", video_->height_);
    if (!video_->mime_type_.empty()) {
      object("mime_type", video_->mime_type_);
    }
    client_->json_store_file(object, video_->video_.get());
  }

 private:
  const td_api::photo *photo_;
  const td_api::video *video_;
  const Client *client_;
};

class Client::JsonChatPhoto final : public td::Jsonable {
 public:
  JsonChatPhoto(const td_api::chatPhoto *photo, const Client *client) : photo_(photo), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &photo_size : photo_->sizes_) {
      if (photo_size->type_ != "i" && photo_size->type_ != "t" && !photo_size->photo_->remote_->id_.empty()) {
        array << JsonPhotoSize(photo_size.get(), client_);
      }
    }
  }

 private:
  const td_api::chatPhoto *photo_;
  const Client *client_;
};

class Client::JsonVideoQuality final : public td::Jsonable {
 public:
  JsonVideoQuality(const td_api::alternativeVideo *alternative_video, const Client *client)
      : alternative_video_(alternative_video), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("width", alternative_video_->width_);
    object("height", alternative_video_->height_);
    object("codec", alternative_video_->codec_);
    client_->json_store_file(object, alternative_video_->video_.get());
  }

 private:
  const td_api::alternativeVideo *alternative_video_;
  const Client *client_;
};

class Client::JsonVideo final : public td::Jsonable {
 public:
  JsonVideo(const td_api::video *video, const td_api::photo *cover, int32 start_timestamp,
            const td::vector<object_ptr<td_api::alternativeVideo>> *alternative_videos, const Client *client)
      : video_(video)
      , cover_(cover)
      , start_timestamp_(start_timestamp)
      , alternative_videos_(alternative_videos)
      , client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("duration", video_->duration_);
    object("width", video_->width_);
    object("height", video_->height_);
    if (!video_->file_name_.empty()) {
      object("file_name", video_->file_name_);
    }
    if (!video_->mime_type_.empty()) {
      object("mime_type", video_->mime_type_);
    }
    if (cover_ != nullptr) {
      object("cover", JsonPhoto(cover_, client_));
    }
    if (start_timestamp_ > 0) {
      object("start_timestamp", start_timestamp_);
    }
    if (alternative_videos_ != nullptr && !alternative_videos_->empty()) {
      object("qualities",
             td::json_array(*alternative_videos_,
                            [client = client_](const object_ptr<td_api::alternativeVideo> &alternative_video) {
                              return JsonVideoQuality(alternative_video.get(), client);
                            }));
    }
    client_->json_store_thumbnail(object, video_->thumbnail_.get());
    client_->json_store_file(object, video_->video_.get());
  }

 private:
  const td_api::video *video_;
  const td_api::photo *cover_;
  int32 start_timestamp_;
  const td::vector<object_ptr<td_api::alternativeVideo>> *alternative_videos_;
  const Client *client_;
};

class Client::JsonVideoNote final : public td::Jsonable {
 public:
  JsonVideoNote(const td_api::videoNote *video_note, const Client *client) : video_note_(video_note), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("duration", video_note_->duration_);
    object("length", video_note_->length_);
    client_->json_store_thumbnail(object, video_note_->thumbnail_.get());
    client_->json_store_file(object, video_note_->video_.get());
  }

 private:
  const td_api::videoNote *video_note_;
  const Client *client_;
};

class Client::JsonVoiceNote final : public td::Jsonable {
 public:
  JsonVoiceNote(const td_api::voiceNote *voice_note, const Client *client) : voice_note_(voice_note), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("duration", voice_note_->duration_);
    if (!voice_note_->mime_type_.empty()) {
      object("mime_type", voice_note_->mime_type_);
    }
    client_->json_store_file(object, voice_note_->voice_.get());
  }

 private:
  const td_api::voiceNote *voice_note_;
  const Client *client_;
};

class Client::JsonPaidMedia final : public td::Jsonable {
 public:
  JsonPaidMedia(const td_api::PaidMedia *paid_media, const Client *client) : paid_media_(paid_media), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    switch (paid_media_->get_id()) {
      case td_api::paidMediaPreview::ID: {
        auto media = static_cast<const td_api::paidMediaPreview *>(paid_media_);
        object("type", "preview");
        if (media->width_) {
          object("width", media->width_);
        }
        if (media->height_) {
          object("height", media->height_);
        }
        if (media->duration_) {
          object("duration", media->duration_);
        }
        break;
      }
      case td_api::paidMediaPhoto::ID: {
        auto media = static_cast<const td_api::paidMediaPhoto *>(paid_media_);
        if (media->video_ != nullptr) {
          object("type", "live_photo");
          object("live_photo", JsonLivePhoto(media->photo_.get(), media->video_.get(), client_));
        } else {
          object("type", "photo");
          object("photo", JsonPhoto(media->photo_.get(), client_));
        }
        break;
      }
      case td_api::paidMediaVideo::ID: {
        auto media = static_cast<const td_api::paidMediaVideo *>(paid_media_);
        object("type", "video");
        object("video", JsonVideo(media->video_.get(), media->cover_.get(), media->start_timestamp_, nullptr, client_));
        break;
      }
      case td_api::paidMediaUnsupported::ID:
        object("type", "other");
        break;
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::PaidMedia *paid_media_;
  const Client *client_;
};

class Client::JsonPaidMediaInfo final : public td::Jsonable {
 public:
  JsonPaidMediaInfo(const td_api::messagePaidMedia *paid_media, const Client *client)
      : paid_media_(paid_media), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("star_count", paid_media_->star_count_);
    object("paid_media", td::json_array(paid_media_->media_, [client = client_](auto &media) {
             return JsonPaidMedia(media.get(), client);
           }));
  }

 private:
  const td_api::messagePaidMedia *paid_media_;
  const Client *client_;
};

class Client::JsonVenue final : public td::Jsonable {
 public:
  explicit JsonVenue(const td_api::venue *venue) : venue_(venue) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("location", JsonLocation(venue_->location_.get()));
    object("title", venue_->title_);
    object("address", venue_->address_);
    if (venue_->provider_ == "foursquare") {
      if (!venue_->id_.empty()) {
        object("foursquare_id", venue_->id_);
      }
      if (!venue_->type_.empty()) {
        object("foursquare_type", venue_->type_);
      }
    }
    if (venue_->provider_ == "gplaces") {
      if (!venue_->id_.empty()) {
        object("google_place_id", venue_->id_);
      }
      if (!venue_->type_.empty()) {
        object("google_place_type", venue_->type_);
      }
    }
  }

 private:
  const td_api::venue *venue_;
};

class Client::JsonContact final : public td::Jsonable {
 public:
  explicit JsonContact(const td_api::contact *contact) : contact_(contact) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("phone_number", contact_->phone_number_);
    object("first_name", contact_->first_name_);
    if (!contact_->last_name_.empty()) {
      object("last_name", contact_->last_name_);
    }
    if (!contact_->vcard_.empty()) {
      object("vcard", contact_->vcard_);
    }
    if (contact_->user_id_) {
      object("user_id", contact_->user_id_);
    }
  }

 private:
  const td_api::contact *contact_;
};

class Client::JsonDice final : public td::Jsonable {
 public:
  JsonDice(const td::string &emoji, int32 value) : emoji_(emoji), value_(value) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("emoji", emoji_);
    object("value", value_);
  }

 private:
  const td::string &emoji_;
  int32 value_;
};

class Client::JsonGame final : public td::Jsonable {
 public:
  JsonGame(const td_api::game *game, const Client *client) : game_(game), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("title", game_->title_);
    if (!game_->text_->text_.empty()) {
      object("text", game_->text_->text_);
    }
    if (!game_->text_->entities_.empty()) {
      object("text_entities", JsonVectorEntities(game_->text_->entities_, client_));
    }
    object("description", game_->description_);
    CHECK(game_->photo_ != nullptr);
    object("photo", JsonPhoto(game_->photo_.get(), client_));
    if (game_->animation_ != nullptr) {
      object("animation", JsonAnimation(game_->animation_.get(), false, client_));
    }
  }

 private:
  const td_api::game *game_;
  const Client *client_;
};

class Client::JsonInvoice final : public td::Jsonable {
 public:
  explicit JsonInvoice(const td_api::messageInvoice *invoice) : invoice_(invoice) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("title", invoice_->product_info_->title_);
    object("description", invoice_->product_info_->description_->text_);
    object("start_parameter", invoice_->start_parameter_);
    object("currency", invoice_->currency_);
    object("total_amount", invoice_->total_amount_);
    // skip product_info_->photo
    // skip is_test
    // skip need_shipping_address
    // skip receipt_message_id
  }

 private:
  const td_api::messageInvoice *invoice_;
};

class Client::JsonPollMedia final : public td::Jsonable {
 public:
  JsonPollMedia(const td_api::PollMedia *media, const Client *client) : media_(media), client_(client) {
    CHECK(media_ != nullptr);
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    switch (media_->get_id()) {
      case td_api::pollMediaAnimation::ID: {
        auto media = static_cast<const td_api::pollMediaAnimation *>(media_);
        object("animation", JsonAnimation(media->animation_.get(), false, client_));
        break;
      }
      case td_api::pollMediaAudio::ID: {
        auto media = static_cast<const td_api::pollMediaAudio *>(media_);
        object("audio", JsonAudio(media->audio_.get(), client_));
        break;
      }
      case td_api::pollMediaDocument::ID: {
        auto media = static_cast<const td_api::pollMediaDocument *>(media_);
        object("document", JsonDocument(media->document_.get(), client_));
        break;
      }
      case td_api::pollMediaLink::ID: {
        auto media = static_cast<const td_api::pollMediaLink *>(media_);
        object("link", JsonLink(media->url_));
        break;
      }
      case td_api::pollMediaLocation::ID: {
        auto media = static_cast<const td_api::pollMediaLocation *>(media_);
        object("location", JsonLocation(media->location_.get()));
        break;
      }
      case td_api::pollMediaPhoto::ID: {
        auto media = static_cast<const td_api::pollMediaPhoto *>(media_);
        if (media->video_ != nullptr) {
          object("live_photo", JsonLivePhoto(media->photo_.get(), media->video_.get(), client_));
        } else {
          object("photo", JsonPhoto(media->photo_.get(), client_));
        }
        break;
      }
      case td_api::pollMediaSticker::ID: {
        auto media = static_cast<const td_api::pollMediaSticker *>(media_);
        object("sticker", JsonSticker(media->sticker_.get(), client_));
        break;
      }
      case td_api::pollMediaVenue::ID: {
        auto media = static_cast<const td_api::pollMediaVenue *>(media_);
        object("venue", JsonVenue(media->venue_.get()));
        break;
      }
      case td_api::pollMediaVideo::ID: {
        auto media = static_cast<const td_api::pollMediaVideo *>(media_);
        object("video", JsonVideo(media->video_.get(), media->cover_.get(), media->start_timestamp_,
                                  &media->alternative_videos_, client_));
        break;
      }
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::PollMedia *media_;
  const Client *client_;
};

class Client::JsonPollOption final : public td::Jsonable {
 public:
  JsonPollOption(const td_api::pollOption *option, const Client *client) : option_(option), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("persistent_id", option_->id_);
    object("text", option_->text_->text_);
    if (!option_->text_->entities_.empty()) {
      object("text_entities", JsonVectorEntities(option_->text_->entities_, client_));
    }
    if (option_->media_ != nullptr) {
      object("media", JsonPollMedia(option_->media_.get(), client_));
    }
    object("voter_count", option_->voter_count_);
    if (option_->author_ != nullptr && option_->addition_date_ > 0) {
      client_->json_store_message_sender(object, option_->author_, "added_by_user", "added_by_chat");
      object("addition_date", option_->addition_date_);
    }
  }

 private:
  const td_api::pollOption *option_;
  const Client *client_;
};

class Client::JsonPoll final : public td::Jsonable {
 public:
  JsonPoll(const td_api::poll *poll, const td_api::formattedText *description, const td_api::PollMedia *media,
           const Client *client)
      : poll_(poll), description_(description), media_(media), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(poll_->id_));
    object("question", poll_->question_->text_);
    if (!poll_->question_->entities_.empty()) {
      object("question_entities", JsonVectorEntities(poll_->question_->entities_, client_));
    }
    object("options", td::json_array(poll_->options_, [client = client_](auto &option) {
             return JsonPollOption(option.get(), client);
           }));
    object("total_voter_count", poll_->total_voter_count_);
    if (poll_->open_period_ != 0 && poll_->close_date_ != 0) {
      object("open_period", poll_->open_period_);
      object("close_date", poll_->close_date_);
    }
    object("is_closed", td::JsonBool(poll_->is_closed_));
    object("is_anonymous", td::JsonBool(poll_->is_anonymous_));
    object("allows_multiple_answers", td::JsonBool(poll_->allows_multiple_answers_));
    object("allows_revoting", td::JsonBool(poll_->allows_revoting_));
    object("members_only", td::JsonBool(poll_->members_only_));
    if (!poll_->country_codes_.empty()) {
      object("country_codes",
             td::json_array(poll_->country_codes_, [](const td::string &country_code) { return country_code; }));
    }
    switch (poll_->type_->get_id()) {
      case td_api::pollTypeQuiz::ID: {
        object("type", "quiz");
        auto quiz = static_cast<const td_api::pollTypeQuiz *>(poll_->type_.get());
        if (quiz->correct_option_ids_.size() == 1u) {
          object("correct_option_id", quiz->correct_option_ids_[0]);
        }
        if (!quiz->correct_option_ids_.empty()) {
          object("correct_option_ids",
                 td::json_array(quiz->correct_option_ids_, [](int32 option_id) { return option_id; }));
        }
        auto *explanation = quiz->explanation_.get();
        if (!explanation->text_.empty()) {
          object("explanation", explanation->text_);
          object("explanation_entities", JsonVectorEntities(explanation->entities_, client_));
        }
        if (quiz->explanation_media_ != nullptr) {
          object("explanation_media", JsonPollMedia(quiz->explanation_media_.get(), client_));
        }
        break;
      }
      case td_api::pollTypeRegular::ID:
        object("type", "regular");
        break;
      default:
        UNREACHABLE();
    }
    if (description_ != nullptr && !description_->text_.empty()) {
      object("description", description_->text_);
      if (!description_->entities_.empty()) {
        object("description_entities", JsonVectorEntities(description_->entities_, client_));
      }
    }
    if (media_ != nullptr) {
      object("media", JsonPollMedia(media_, client_));
    }
  }

 private:
  const td_api::poll *poll_;
  const td_api::formattedText *description_;
  const td_api::PollMedia *media_;
  const Client *client_;
};

class Client::JsonPollAnswer final : public td::Jsonable {
 public:
  JsonPollAnswer(const td_api::updatePollAnswer *poll_answer, const Client *client)
      : poll_answer_(poll_answer), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("poll_id", td::to_string(poll_answer_->poll_id_));
    client_->json_store_message_sender(object, poll_answer_->voter_id_, "user", "voter_chat",
                                       client_->channel_bot_user_id_);
    object("option_ids", td::json_array(poll_answer_->option_positions_, [](int32 option_id) { return option_id; }));
    object("option_persistent_ids",
           td::json_array(poll_answer_->option_ids_, [](const td::string &option_id) { return option_id; }));
  }

 private:
  const td_api::updatePollAnswer *poll_answer_;
  const Client *client_;
};

class Client::JsonPollOptionAdded final : public td::Jsonable {
 public:
  JsonPollOptionAdded(const td_api::messagePollOptionAdded *poll_option_added, int64 chat_id, bool need_reply,
                      const Client *client)
      : poll_option_added_(poll_option_added), chat_id_(chat_id), need_reply_(need_reply), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    const MessageInfo *poll_message = client_->get_message(chat_id_, poll_option_added_->poll_message_id_, true);
    if (poll_message != nullptr) {
      object("poll_message", JsonMessage(poll_message, false, "poll option added", client_));
    } else if (need_reply_) {
      object("poll_message", JsonInaccessibleMessage(chat_id_, poll_option_added_->poll_message_id_, client_));
    }
    object("option_persistent_id", poll_option_added_->option_id_);
    object("option_text", poll_option_added_->text_->text_);
    if (!poll_option_added_->text_->entities_.empty()) {
      object("option_entities", JsonVectorEntities(poll_option_added_->text_->entities_, client_));
    }
  }

 private:
  const td_api::messagePollOptionAdded *poll_option_added_;
  int64 chat_id_;
  bool need_reply_;
  const Client *client_;
};

class Client::JsonPollOptionDeleted final : public td::Jsonable {
 public:
  JsonPollOptionDeleted(const td_api::messagePollOptionDeleted *poll_option_deleted, int64 chat_id, bool need_reply,
                        const Client *client)
      : poll_option_deleted_(poll_option_deleted), chat_id_(chat_id), need_reply_(need_reply), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    const MessageInfo *poll_message = client_->get_message(chat_id_, poll_option_deleted_->poll_message_id_, true);
    if (poll_message != nullptr) {
      object("poll_message", JsonMessage(poll_message, false, "poll option deleted", client_));
    } else if (need_reply_) {
      object("poll_message", JsonInaccessibleMessage(chat_id_, poll_option_deleted_->poll_message_id_, client_));
    }
    object("option_persistent_id", poll_option_deleted_->option_id_);
    object("option_text", poll_option_deleted_->text_->text_);
    if (!poll_option_deleted_->text_->entities_.empty()) {
      object("option_entities", JsonVectorEntities(poll_option_deleted_->text_->entities_, client_));
    }
  }

 private:
  const td_api::messagePollOptionDeleted *poll_option_deleted_;
  int64 chat_id_;
  bool need_reply_;
  const Client *client_;
};

class Client::JsonChecklistTask final : public td::Jsonable {
 public:
  JsonChecklistTask(const td_api::checklistTask *task, const Client *client) : task_(task), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", task_->id_);
    object("text", task_->text_->text_);
    if (!task_->text_->entities_.empty()) {
      object("text_entities", JsonVectorEntities(task_->text_->entities_, client_));
    }
    if (task_->completed_by_ != nullptr && task_->completion_date_ != 0) {
      client_->json_store_message_sender(object, task_->completed_by_, "completed_by_user", "completed_by_chat");
      object("completion_date", task_->completion_date_);
    }
  }

 private:
  const td_api::checklistTask *task_;
  const Client *client_;
};

class Client::JsonChecklist final : public td::Jsonable {
 public:
  JsonChecklist(const td_api::checklist *checklist, const Client *client) : checklist_(checklist), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("title", checklist_->title_->text_);
    if (!checklist_->title_->entities_.empty()) {
      object("title_entities", JsonVectorEntities(checklist_->title_->entities_, client_));
    }
    object("tasks", td::json_array(checklist_->tasks_,
                                   [client = client_](auto &task) { return JsonChecklistTask(task.get(), client); }));
    if (checklist_->others_can_add_tasks_) {
      object("others_can_add_tasks", td::JsonTrue());
    }
    if (checklist_->others_can_mark_tasks_as_done_) {
      object("others_can_mark_tasks_as_done", td::JsonTrue());
    }
  }

 private:
  const td_api::checklist *checklist_;
  const Client *client_;
};

class Client::JsonChecklistTasksDone final : public td::Jsonable {
 public:
  JsonChecklistTasksDone(const td_api::messageChecklistTasksDone *checklist_tasks_done, int64 chat_id,
                         const Client *client)
      : checklist_tasks_done_(checklist_tasks_done), chat_id_(chat_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    const MessageInfo *checklist_message =
        client_->get_message(chat_id_, checklist_tasks_done_->checklist_message_id_, true);
    if (checklist_message != nullptr) {
      object("checklist_message", JsonMessage(checklist_message, false, "checklist tasks done", client_));
    }
    if (!checklist_tasks_done_->marked_as_done_task_ids_.empty()) {
      object("marked_as_done_task_ids",
             td::json_array(checklist_tasks_done_->marked_as_done_task_ids_, [](int32 task_id) { return task_id; }));
    }
    if (!checklist_tasks_done_->marked_as_not_done_task_ids_.empty()) {
      object("marked_as_not_done_task_ids", td::json_array(checklist_tasks_done_->marked_as_not_done_task_ids_,
                                                           [](int32 task_id) { return task_id; }));
    }
  }

 private:
  const td_api::messageChecklistTasksDone *checklist_tasks_done_;
  int64 chat_id_;
  const Client *client_;
};

class Client::JsonChecklistTasksAdded final : public td::Jsonable {
 public:
  JsonChecklistTasksAdded(const td_api::messageChecklistTasksAdded *checklist_tasks_added, int64 chat_id,
                          const Client *client)
      : checklist_tasks_added_(checklist_tasks_added), chat_id_(chat_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    const MessageInfo *checklist_message =
        client_->get_message(chat_id_, checklist_tasks_added_->checklist_message_id_, true);
    if (checklist_message != nullptr) {
      object("checklist_message", JsonMessage(checklist_message, false, "checklist tasks added", client_));
    }
    object("tasks", td::json_array(checklist_tasks_added_->tasks_,
                                   [client = client_](auto &task) { return JsonChecklistTask(task.get(), client); }));
  }

 private:
  const td_api::messageChecklistTasksAdded *checklist_tasks_added_;
  int64 chat_id_;
  const Client *client_;
};

class Client::JsonSuggestedPostApprovalFailed final : public td::Jsonable {
 public:
  JsonSuggestedPostApprovalFailed(const td_api::messageSuggestedPostApprovalFailed *suggested_post_approval_failed,
                                  int64 chat_id, const Client *client)
      : suggested_post_approval_failed_(suggested_post_approval_failed), chat_id_(chat_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    const MessageInfo *suggested_post_message =
        client_->get_message(chat_id_, suggested_post_approval_failed_->suggested_post_message_id_, true);
    if (suggested_post_message != nullptr) {
      object("suggested_post_message",
             JsonMessage(suggested_post_message, false, "suggested post approval failed", client_));
    }
    object("price", JsonSuggestedPostPrice(suggested_post_approval_failed_->price_.get()));
  }

 private:
  const td_api::messageSuggestedPostApprovalFailed *suggested_post_approval_failed_;
  int64 chat_id_;
  const Client *client_;
};

class Client::JsonSuggestedPostApproved final : public td::Jsonable {
 public:
  JsonSuggestedPostApproved(const td_api::messageSuggestedPostApproved *suggested_post_approved, int64 chat_id,
                            const Client *client)
      : suggested_post_approved_(suggested_post_approved), chat_id_(chat_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    const MessageInfo *suggested_post_message =
        client_->get_message(chat_id_, suggested_post_approved_->suggested_post_message_id_, true);
    if (suggested_post_message != nullptr) {
      object("suggested_post_message", JsonMessage(suggested_post_message, false, "suggested post approved", client_));
    }
    if (suggested_post_approved_->price_ != nullptr) {
      object("price", JsonSuggestedPostPrice(suggested_post_approved_->price_.get()));
    }
    object("send_date", suggested_post_approved_->send_date_);
  }

 private:
  const td_api::messageSuggestedPostApproved *suggested_post_approved_;
  int64 chat_id_;
  const Client *client_;
};

class Client::JsonSuggestedPostDeclined final : public td::Jsonable {
 public:
  JsonSuggestedPostDeclined(const td_api::messageSuggestedPostDeclined *suggested_post_declined, int64 chat_id,
                            const Client *client)
      : suggested_post_declined_(suggested_post_declined), chat_id_(chat_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    const MessageInfo *suggested_post_message =
        client_->get_message(chat_id_, suggested_post_declined_->suggested_post_message_id_, true);
    if (suggested_post_message != nullptr) {
      object("suggested_post_message", JsonMessage(suggested_post_message, false, "suggested post declined", client_));
    }
    if (!suggested_post_declined_->comment_.empty()) {
      object("comment", suggested_post_declined_->comment_);
    }
  }

 private:
  const td_api::messageSuggestedPostDeclined *suggested_post_declined_;
  int64 chat_id_;
  const Client *client_;
};

class Client::JsonSuggestedPostPaid final : public td::Jsonable {
 public:
  JsonSuggestedPostPaid(const td_api::messageSuggestedPostPaid *suggested_post_paid, int64 chat_id,
                        const Client *client)
      : suggested_post_paid_(suggested_post_paid), chat_id_(chat_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    const MessageInfo *suggested_post_message =
        client_->get_message(chat_id_, suggested_post_paid_->suggested_post_message_id_, true);
    if (suggested_post_message != nullptr) {
      object("suggested_post_message", JsonMessage(suggested_post_message, false, "suggested post paid", client_));
    }
    if (suggested_post_paid_->star_amount_->star_count_ != 0 ||
        suggested_post_paid_->star_amount_->nanostar_count_ != 0) {
      object("currency", "XTR");
      object("star_amount", JsonStarAmount(suggested_post_paid_->star_amount_.get()));
    } else {
      object("currency", "TON");
      object("amount", suggested_post_paid_->gram_amount_);
    }
  }

 private:
  const td_api::messageSuggestedPostPaid *suggested_post_paid_;
  int64 chat_id_;
  const Client *client_;
};

class Client::JsonSuggestedPostRefunded final : public td::Jsonable {
 public:
  JsonSuggestedPostRefunded(const td_api::messageSuggestedPostRefunded *suggested_post_refunded, int64 chat_id,
                            const Client *client)
      : suggested_post_refunded_(suggested_post_refunded), chat_id_(chat_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    const MessageInfo *suggested_post_message =
        client_->get_message(chat_id_, suggested_post_refunded_->suggested_post_message_id_, true);
    if (suggested_post_message != nullptr) {
      object("suggested_post_message", JsonMessage(suggested_post_message, false, "suggested post refunded", client_));
    }
    switch (suggested_post_refunded_->reason_->get_id()) {
      case td_api::suggestedPostRefundReasonPostDeleted::ID:
        object("reason", "post_deleted");
        break;
      case td_api::suggestedPostRefundReasonPaymentRefunded::ID:
        object("reason", "payment_refunded");
        break;
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::messageSuggestedPostRefunded *suggested_post_refunded_;
  int64 chat_id_;
  const Client *client_;
};

class Client::JsonStory final : public td::Jsonable {
 public:
  JsonStory(int64 chat_id, int32 story_id, const Client *client)
      : chat_id_(chat_id), story_id_(story_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("chat", JsonChat(chat_id_, client_));
    object("id", story_id_);
  }

 private:
  int64 chat_id_;
  int32 story_id_;
  const Client *client_;
};

class Client::JsonBackgroundFill final : public td::Jsonable {
 public:
  explicit JsonBackgroundFill(const td_api::BackgroundFill *background_fill) : background_fill_(background_fill) {
  }
  void store(td::JsonValueScope *scope) const {
    CHECK(background_fill_ != nullptr);
    auto object = scope->enter_object();
    switch (background_fill_->get_id()) {
      case td_api::backgroundFillSolid::ID: {
        auto fill = static_cast<const td_api::backgroundFillSolid *>(background_fill_);
        object("type", "solid");
        object("color", fill->color_);
        break;
      }
      case td_api::backgroundFillGradient::ID: {
        auto fill = static_cast<const td_api::backgroundFillGradient *>(background_fill_);
        object("type", "gradient");
        object("top_color", fill->top_color_);
        object("bottom_color", fill->bottom_color_);
        object("rotation_angle", fill->rotation_angle_);
        break;
      }
      case td_api::backgroundFillFreeformGradient::ID: {
        auto fill = static_cast<const td_api::backgroundFillFreeformGradient *>(background_fill_);
        object("type", "freeform_gradient");
        object("colors", td::json_array(fill->colors_, [](int32 color) { return color; }));
        break;
      }
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::BackgroundFill *background_fill_;
};

class Client::JsonBackgroundType final : public td::Jsonable {
 public:
  JsonBackgroundType(const td_api::BackgroundType *background_type, const td_api::document *document,
                     int32 dark_theme_dimming, const Client *client)
      : background_type_(background_type)
      , document_(document)
      , dark_theme_dimming_(dark_theme_dimming)
      , client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    CHECK(background_type_ != nullptr);
    auto object = scope->enter_object();
    switch (background_type_->get_id()) {
      case td_api::backgroundTypeWallpaper::ID: {
        auto type = static_cast<const td_api::backgroundTypeWallpaper *>(background_type_);
        object("type", "wallpaper");
        CHECK(document_ != nullptr);
        object("document", JsonDocument(document_, client_));
        object("dark_theme_dimming", dark_theme_dimming_);
        if (type->is_blurred_) {
          object("is_blurred", td::JsonTrue());
        }
        if (type->is_moving_) {
          object("is_moving", td::JsonTrue());
        }
        break;
      }
      case td_api::backgroundTypePattern::ID: {
        auto type = static_cast<const td_api::backgroundTypePattern *>(background_type_);
        object("type", "pattern");
        CHECK(document_ != nullptr);
        object("document", JsonDocument(document_, client_));
        object("fill", JsonBackgroundFill(type->fill_.get()));
        object("intensity", type->intensity_);
        if (type->is_inverted_) {
          object("is_inverted", td::JsonTrue());
        }
        if (type->is_moving_) {
          object("is_moving", td::JsonTrue());
        }
        break;
      }
      case td_api::backgroundTypeFill::ID: {
        auto type = static_cast<const td_api::backgroundTypeFill *>(background_type_);
        object("type", "fill");
        object("fill", JsonBackgroundFill(type->fill_.get()));
        object("dark_theme_dimming", dark_theme_dimming_);
        break;
      }
      case td_api::backgroundTypeChatTheme::ID: {
        auto type = static_cast<const td_api::backgroundTypeChatTheme *>(background_type_);
        object("type", "chat_theme");
        object("theme_name", type->theme_name_);
        break;
      }
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::BackgroundType *background_type_;
  const td_api::document *document_;
  int32 dark_theme_dimming_;
  const Client *client_;
};

class Client::JsonChatBackground final : public td::Jsonable {
 public:
  JsonChatBackground(const td_api::chatBackground *chat_background, const Client *client)
      : chat_background_(chat_background), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    CHECK(chat_background_ != nullptr);
    auto object = scope->enter_object();
    auto background = chat_background_->background_.get();
    object("type", JsonBackgroundType(background->type_.get(), background->document_.get(),
                                      chat_background_->dark_theme_dimming_, client_));
  }

 private:
  const td_api::chatBackground *chat_background_;
  const Client *client_;
};

class Client::JsonChatOwnerChanged final : public td::Jsonable {
 public:
  JsonChatOwnerChanged(const td_api::messageChatOwnerChanged *chat_owner_changed, const Client *client)
      : chat_owner_changed_(chat_owner_changed), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("new_owner", JsonUser(chat_owner_changed_->new_owner_user_id_, client_));
  }

 private:
  const td_api::messageChatOwnerChanged *chat_owner_changed_;
  const Client *client_;
};

class Client::JsonChatOwnerLeft final : public td::Jsonable {
 public:
  JsonChatOwnerLeft(const td_api::messageChatOwnerLeft *chat_owner_left, const Client *client)
      : chat_owner_left_(chat_owner_left), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (chat_owner_left_->new_owner_user_id_ > 0) {
      object("new_owner", JsonUser(chat_owner_left_->new_owner_user_id_, client_));
    }
  }

 private:
  const td_api::messageChatOwnerLeft *chat_owner_left_;
  const Client *client_;
};

class Client::JsonForumTopicCreated final : public td::Jsonable {
 public:
  explicit JsonForumTopicCreated(const td_api::messageForumTopicCreated *forum_topic_created)
      : forum_topic_created_(forum_topic_created) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("name", forum_topic_created_->name_);
    object("icon_color", forum_topic_created_->icon_->color_);
    if (forum_topic_created_->icon_->custom_emoji_id_ != 0) {
      object("icon_custom_emoji_id", td::to_string(forum_topic_created_->icon_->custom_emoji_id_));
    }
    if (forum_topic_created_->is_name_implicit_) {
      object("is_name_implicit", td::JsonTrue());
    }
  }

 private:
  const td_api::messageForumTopicCreated *forum_topic_created_;
};

class Client::JsonForumTopicEdited final : public td::Jsonable {
 public:
  explicit JsonForumTopicEdited(const td_api::messageForumTopicEdited *forum_topic_edited)
      : forum_topic_edited_(forum_topic_edited) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (!forum_topic_edited_->name_.empty()) {
      object("name", forum_topic_edited_->name_);
    }
    if (forum_topic_edited_->edit_icon_custom_emoji_id_) {
      object("icon_custom_emoji_id", forum_topic_edited_->icon_custom_emoji_id_ == 0
                                         ? td::string()
                                         : td::to_string(forum_topic_edited_->icon_custom_emoji_id_));
    }
  }

 private:
  const td_api::messageForumTopicEdited *forum_topic_edited_;
};

class Client::JsonForumTopicInfo final : public td::Jsonable {
 public:
  explicit JsonForumTopicInfo(const td_api::forumTopicInfo *forum_topic_info) : forum_topic_info_(forum_topic_info) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("message_thread_id", forum_topic_info_->forum_topic_id_);
    object("name", forum_topic_info_->name_);
    object("icon_color", forum_topic_info_->icon_->color_);
    if (forum_topic_info_->icon_->custom_emoji_id_ != 0) {
      object("icon_custom_emoji_id", td::to_string(forum_topic_info_->icon_->custom_emoji_id_));
    }
  }

 private:
  const td_api::forumTopicInfo *forum_topic_info_;
};

class Client::JsonAddress final : public td::Jsonable {
 public:
  explicit JsonAddress(const td_api::address *address) : address_(address) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("country_code", address_->country_code_);
    object("state", address_->state_);
    object("city", address_->city_);
    object("street_line1", address_->street_line1_);
    object("street_line2", address_->street_line2_);
    object("post_code", address_->postal_code_);
  }

 private:
  const td_api::address *address_;
};

class Client::JsonOrderInfo final : public td::Jsonable {
 public:
  explicit JsonOrderInfo(const td_api::orderInfo *order_info) : order_info_(order_info) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (!order_info_->name_.empty()) {
      object("name", order_info_->name_);
    }
    if (!order_info_->phone_number_.empty()) {
      object("phone_number", order_info_->phone_number_);
    }
    if (!order_info_->email_address_.empty()) {
      object("email", order_info_->email_address_);
    }
    if (order_info_->shipping_address_ != nullptr) {
      object("shipping_address", JsonAddress(order_info_->shipping_address_.get()));
    }
  }

 private:
  const td_api::orderInfo *order_info_;
};

class Client::JsonSuccessfulPaymentBot final : public td::Jsonable {
 public:
  explicit JsonSuccessfulPaymentBot(const td_api::messagePaymentSuccessfulBot *successful_payment)
      : successful_payment_(successful_payment) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("currency", successful_payment_->currency_);
    object("total_amount", successful_payment_->total_amount_);
    if (!td::check_utf8(successful_payment_->invoice_payload_)) {
      LOG(WARNING) << "Receive non-UTF-8 invoice payload";
      object("invoice_payload", td::JsonRawString(successful_payment_->invoice_payload_));
    } else {
      object("invoice_payload", successful_payment_->invoice_payload_);
    }
    if (!successful_payment_->shipping_option_id_.empty()) {
      object("shipping_option_id", successful_payment_->shipping_option_id_);
    }
    if (successful_payment_->order_info_ != nullptr) {
      object("order_info", JsonOrderInfo(successful_payment_->order_info_.get()));
    }
    object("telegram_payment_charge_id", successful_payment_->telegram_payment_charge_id_);
    object("provider_payment_charge_id", successful_payment_->provider_payment_charge_id_);
    if (successful_payment_->is_recurring_) {
      object("is_recurring", td::JsonTrue());
    }
    if (successful_payment_->is_first_recurring_) {
      object("is_first_recurring", td::JsonTrue());
    }
    if (successful_payment_->subscription_until_date_ > 0) {
      object("subscription_expiration_date", successful_payment_->subscription_until_date_);
    }
  }

 private:
  const td_api::messagePaymentSuccessfulBot *successful_payment_;
};

class Client::JsonRefundedPayment final : public td::Jsonable {
 public:
  explicit JsonRefundedPayment(const td_api::messagePaymentRefunded *refunded_payment)
      : refunded_payment_(refunded_payment) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("currency", refunded_payment_->currency_);
    object("total_amount", refunded_payment_->total_amount_);
    if (!td::check_utf8(refunded_payment_->invoice_payload_)) {
      LOG(WARNING) << "Receive non-UTF-8 invoice payload";
      object("invoice_payload", td::JsonRawString(refunded_payment_->invoice_payload_));
    } else {
      object("invoice_payload", refunded_payment_->invoice_payload_);
    }
    object("telegram_payment_charge_id", refunded_payment_->telegram_payment_charge_id_);
    if (!refunded_payment_->provider_payment_charge_id_.empty()) {
      object("provider_payment_charge_id", refunded_payment_->provider_payment_charge_id_);
    }
  }

 private:
  const td_api::messagePaymentRefunded *refunded_payment_;
};

class Client::JsonGiftMessage final : public td::Jsonable {
 public:
  JsonGiftMessage(const td_api::messageGift *gift, const Client *client) : gift_(gift), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("gift", JsonGift(gift_->gift_.get(), client_));
    if (!gift_->received_gift_id_.empty()) {
      object("owned_gift_id", gift_->received_gift_id_);
    }
    if (gift_->sell_star_count_ > 0) {
      object("convert_star_count", gift_->sell_star_count_);
    }
    if (gift_->prepaid_upgrade_star_count_ > 0) {
      object("prepaid_upgrade_star_count", gift_->prepaid_upgrade_star_count_);
    }
    if (gift_->can_be_upgraded_) {
      object("can_be_upgraded", td::JsonTrue());
    }
    if (!gift_->text_->text_.empty()) {
      object("text", gift_->text_->text_);
      if (!gift_->text_->entities_.empty()) {
        object("entities", JsonVectorEntities(gift_->text_->entities_, client_));
      }
    }
    if (gift_->is_private_) {
      object("is_private", td::JsonTrue());
    }
    if (gift_->is_upgrade_separate_) {
      object("is_upgrade_separate", td::JsonTrue());
    }
    if (gift_->unique_gift_number_ > 0) {
      object("unique_gift_number", gift_->unique_gift_number_);
    }
  }

 private:
  const td_api::messageGift *gift_;
  const Client *client_;
};

class Client::JsonRefundedUpgradedGiftMessage final : public td::Jsonable {
 public:
  JsonRefundedUpgradedGiftMessage(const td_api::messageRefundedUpgradedGift *gift, const Client *client)
      : gift_(gift), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("gift", JsonGift(gift_->gift_.get(), client_));
  }

 private:
  const td_api::messageRefundedUpgradedGift *gift_;
  const Client *client_;
};

class Client::JsonUniqueGiftMessage final : public td::Jsonable {
 public:
  JsonUniqueGiftMessage(const td_api::messageUpgradedGift *gift, const Client *client) : gift_(gift), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("gift", JsonUniqueGift(gift_->gift_.get(), client_));
    if (!gift_->received_gift_id_.empty()) {
      object("owned_gift_id", gift_->received_gift_id_);
      if (gift_->can_be_transferred_) {
        object("transfer_star_count", gift_->transfer_star_count_);
        if (gift_->next_transfer_date_ > 0) {
          object("next_transfer_date", gift_->next_transfer_date_);
        }
      }
    }
    switch (gift_->origin_->get_id()) {
      case td_api::upgradedGiftOriginUpgrade::ID:
        object("origin", "upgrade");
        break;
      case td_api::upgradedGiftOriginTransfer::ID:
        object("origin", "transfer");
        break;
      case td_api::upgradedGiftOriginResale::ID: {
        auto origin = static_cast<const td_api::upgradedGiftOriginResale *>(gift_->origin_.get());
        object("origin", "resale");
        switch (origin->price_->get_id()) {
          case td_api::giftResalePriceStar::ID: {
            auto price = static_cast<const td_api::giftResalePriceStar *>(origin->price_.get());
            object("last_resale_star_count", price->star_count_);
            object("last_resale_currency", "XTR");
            object("last_resale_amount", price->star_count_);
            break;
          }
          case td_api::giftResalePriceGram::ID: {
            auto price = static_cast<const td_api::giftResalePriceGram *>(origin->price_.get());
            object("last_resale_currency", "TON");
            object("last_resale_amount", price->gram_cent_count_ * 10000000);
            break;
          }
          default:
            UNREACHABLE();
        }
        break;
      }
      case td_api::upgradedGiftOriginPrepaidUpgrade::ID:
        object("origin", "gifted_upgrade");
        break;
      case td_api::upgradedGiftOriginBlockchain::ID:
        LOG(ERROR) << "Receive a gift assigned from blockchain";
        object("origin", "blockchain");
        break;
      case td_api::upgradedGiftOriginOffer::ID:
        object("origin", "offer");
        break;
      case td_api::upgradedGiftOriginCraft::ID:
        LOG(ERROR) << "Receive a crafted gift";
        object("origin", "craft");
        break;
      default:
        UNREACHABLE();
    }
    if (!gift_->text_->text_.empty()) {
      object("text", gift_->text_->text_);
      if (!gift_->text_->entities_.empty()) {
        object("entities", JsonVectorEntities(gift_->text_->entities_, client_));
      }
    }
    if (gift_->is_private_) {
      object("is_private", td::JsonTrue());
    }
  }

 private:
  const td_api::messageUpgradedGift *gift_;
  const Client *client_;
};

class Client::JsonEncryptedPassportElement final : public td::Jsonable {
 public:
  JsonEncryptedPassportElement(const td_api::encryptedPassportElement *element, const Client *client)
      : element_(element), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    auto id = element_->type_->get_id();
    object("type", get_passport_element_type(id));
    switch (id) {
      case td_api::passportElementTypePhoneNumber::ID:
        object("phone_number", element_->value_);
        break;
      case td_api::passportElementTypeEmailAddress::ID:
        object("email", element_->value_);
        break;
      case td_api::passportElementTypePersonalDetails::ID:
      case td_api::passportElementTypePassport::ID:
      case td_api::passportElementTypeDriverLicense::ID:
      case td_api::passportElementTypeIdentityCard::ID:
      case td_api::passportElementTypeInternalPassport::ID:
      case td_api::passportElementTypeAddress::ID:
        object("data", td::base64_encode(element_->data_));
        break;
    }
    switch (id) {
      case td_api::passportElementTypeUtilityBill::ID:
      case td_api::passportElementTypeBankStatement::ID:
      case td_api::passportElementTypeRentalAgreement::ID:
      case td_api::passportElementTypePassportRegistration::ID:
      case td_api::passportElementTypeTemporaryRegistration::ID:
        object("files", JsonDatedFiles(element_->files_, client_));
        if (!element_->translation_.empty()) {
          object("translation", JsonDatedFiles(element_->translation_, client_));
        }
        break;
    }
    switch (id) {
      case td_api::passportElementTypePassport::ID:
      case td_api::passportElementTypeDriverLicense::ID:
      case td_api::passportElementTypeIdentityCard::ID:
      case td_api::passportElementTypeInternalPassport::ID:
        CHECK(element_->front_side_ != nullptr);
        object("front_side", JsonDatedFile(element_->front_side_.get(), client_));
        if (element_->reverse_side_ != nullptr) {
          CHECK(id == td_api::passportElementTypeIdentityCard::ID ||
                id == td_api::passportElementTypeDriverLicense::ID);
          object("reverse_side", JsonDatedFile(element_->reverse_side_.get(), client_));
        } else {
          CHECK(id == td_api::passportElementTypePassport::ID || id == td_api::passportElementTypeInternalPassport::ID);
        }
        if (element_->selfie_ != nullptr) {
          object("selfie", JsonDatedFile(element_->selfie_.get(), client_));
        }
        if (!element_->translation_.empty()) {
          object("translation", JsonDatedFiles(element_->translation_, client_));
        }
        break;
    }
    object("hash", td::base64_encode(element_->hash_));
  }

 private:
  const td_api::encryptedPassportElement *element_;
  const Client *client_;
};

class Client::JsonEncryptedCredentials final : public td::Jsonable {
 public:
  explicit JsonEncryptedCredentials(const td_api::encryptedCredentials *credentials) : credentials_(credentials) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("data", td::base64_encode(credentials_->data_));
    object("hash", td::base64_encode(credentials_->hash_));
    object("secret", td::base64_encode(credentials_->secret_));
  }

 private:
  const td_api::encryptedCredentials *credentials_;
};

class Client::JsonPassportData final : public td::Jsonable {
 public:
  JsonPassportData(const td_api::messagePassportDataReceived *passport_data, const Client *client)
      : passport_data_(passport_data), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("data", td::json_array(passport_data_->elements_, [client = client_](auto &element) {
             return JsonEncryptedPassportElement(element.get(), client);
           }));
    object("credentials", JsonEncryptedCredentials(passport_data_->credentials_.get()));
  }

 private:
  const td_api::messagePassportDataReceived *passport_data_;
  const Client *client_;
};

class Client::JsonWebAppData final : public td::Jsonable {
 public:
  explicit JsonWebAppData(const td_api::messageWebAppDataReceived *web_app_data) : web_app_data_(web_app_data) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("button_text", web_app_data_->button_text_);
    object("data", web_app_data_->data_);
  }

 private:
  const td_api::messageWebAppDataReceived *web_app_data_;
};

class Client::JsonProximityAlertTriggered final : public td::Jsonable {
 public:
  JsonProximityAlertTriggered(const td_api::messageProximityAlertTriggered *proximity_alert_triggered,
                              const Client *client)
      : proximity_alert_triggered_(proximity_alert_triggered), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("traveler", JsonMessageSender(proximity_alert_triggered_->traveler_id_.get(), client_));
    object("watcher", JsonMessageSender(proximity_alert_triggered_->watcher_id_.get(), client_));
    object("distance", proximity_alert_triggered_->distance_);
  }

 private:
  const td_api::messageProximityAlertTriggered *proximity_alert_triggered_;
  const Client *client_;
};

class Client::JsonVideoChatScheduled final : public td::Jsonable {
 public:
  explicit JsonVideoChatScheduled(const td_api::messageVideoChatScheduled *video_chat_scheduled)
      : video_chat_scheduled_(video_chat_scheduled) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("start_date", video_chat_scheduled_->start_date_);
  }

 private:
  const td_api::messageVideoChatScheduled *video_chat_scheduled_;
};

class Client::JsonVideoChatEnded final : public td::Jsonable {
 public:
  explicit JsonVideoChatEnded(const td_api::messageVideoChatEnded *video_chat_ended)
      : video_chat_ended_(video_chat_ended) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("duration", video_chat_ended_->duration_);
  }

 private:
  const td_api::messageVideoChatEnded *video_chat_ended_;
};

class Client::JsonInviteVideoChatParticipants final : public td::Jsonable {
 public:
  JsonInviteVideoChatParticipants(const td_api::messageInviteVideoChatParticipants *invite_video_chat_participants,
                                  const Client *client)
      : invite_video_chat_participants_(invite_video_chat_participants), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("users", JsonUsers(invite_video_chat_participants_->user_ids_, client_));
  }

 private:
  const td_api::messageInviteVideoChatParticipants *invite_video_chat_participants_;
  const Client *client_;
};

class Client::JsonChatSetMessageAutoDeleteTime final : public td::Jsonable {
 public:
  explicit JsonChatSetMessageAutoDeleteTime(
      const td_api::messageChatSetMessageAutoDeleteTime *chat_set_message_auto_delete_time)
      : chat_set_message_auto_delete_time_(chat_set_message_auto_delete_time) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("message_auto_delete_time", chat_set_message_auto_delete_time_->message_auto_delete_time_);
  }

 private:
  const td_api::messageChatSetMessageAutoDeleteTime *chat_set_message_auto_delete_time_;
};

class Client::JsonWriteAccessAllowed final : public td::Jsonable {
 public:
  explicit JsonWriteAccessAllowed(const td_api::messageBotWriteAccessAllowed *write_access_allowed)
      : write_access_allowed_(write_access_allowed) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    switch (write_access_allowed_->reason_->get_id()) {
      case td_api::botWriteAccessAllowReasonLaunchedWebApp::ID:
        object("web_app_name", static_cast<const td_api::botWriteAccessAllowReasonLaunchedWebApp *>(
                                   write_access_allowed_->reason_.get())
                                   ->web_app_->short_name_);
        break;
      case td_api::botWriteAccessAllowReasonAcceptedRequest::ID:
        object("from_request", td::JsonTrue());
        break;
      case td_api::botWriteAccessAllowReasonAddedToAttachmentMenu::ID:
        object("from_attachment_menu", td::JsonTrue());
        break;
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::messageBotWriteAccessAllowed *write_access_allowed_;
};

class Client::JsonUserShared final : public td::Jsonable {
 public:
  explicit JsonUserShared(const td_api::messageUsersShared *users_shared) : users_shared_(users_shared) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("user_id", users_shared_->users_[0]->user_id_);
    object("request_id", users_shared_->button_id_);
  }

 private:
  const td_api::messageUsersShared *users_shared_;
};

class Client::JsonSharedUser final : public td::Jsonable {
 public:
  JsonSharedUser(const td_api::sharedUser *shared_user, const Client *client)
      : shared_user_(shared_user), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("user_id", shared_user_->user_id_);
    if (!shared_user_->first_name_.empty()) {
      object("first_name", shared_user_->first_name_);
    }
    if (!shared_user_->last_name_.empty()) {
      object("last_name", shared_user_->last_name_);
    }
    if (!shared_user_->username_.empty()) {
      object("username", shared_user_->username_);
    }
    if (shared_user_->photo_ != nullptr) {
      object("photo", JsonPhoto(shared_user_->photo_.get(), client_));
    }
  }

 private:
  const td_api::sharedUser *shared_user_;
  const Client *client_;
};

class Client::JsonUsersShared final : public td::Jsonable {
 public:
  JsonUsersShared(const td_api::messageUsersShared *users_shared, const Client *client)
      : users_shared_(users_shared), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("user_ids", td::json_array(users_shared_->users_, [](auto &user) { return user->user_id_; }));
    object("users", td::json_array(users_shared_->users_,
                                   [client = client_](auto &user) { return JsonSharedUser(user.get(), client); }));
    object("request_id", users_shared_->button_id_);
  }

 private:
  const td_api::messageUsersShared *users_shared_;
  const Client *client_;
};

class Client::JsonChatShared final : public td::Jsonable {
 public:
  JsonChatShared(const td_api::messageChatShared *chat_shared, const Client *client)
      : chat_shared_(chat_shared), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    auto *shared_chat = chat_shared_->chat_.get();
    object("chat_id", shared_chat->chat_id_);
    if (!shared_chat->title_.empty()) {
      object("title", shared_chat->title_);
    }
    if (!shared_chat->username_.empty()) {
      object("username", shared_chat->username_);
    }
    if (shared_chat->photo_ != nullptr) {
      object("photo", JsonPhoto(shared_chat->photo_.get(), client_));
    }
    object("request_id", chat_shared_->button_id_);
  }

 private:
  const td_api::messageChatShared *chat_shared_;
  const Client *client_;
};

class Client::JsonManagedBotCreated final : public td::Jsonable {
 public:
  JsonManagedBotCreated(const td_api::messageManagedBotCreated *managed_bot_created, const Client *client)
      : managed_bot_created_(managed_bot_created), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("bot", JsonUser(managed_bot_created_->bot_user_id_, client_));
  }

 private:
  const td_api::messageManagedBotCreated *managed_bot_created_;
  const Client *client_;
};

class Client::JsonManagedBotUpdated final : public td::Jsonable {
 public:
  JsonManagedBotUpdated(const td_api::updateManagedBot *update_managed_bot, const Client *client)
      : update_managed_bot_(update_managed_bot), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("user", JsonUser(update_managed_bot_->user_id_, client_));
    object("bot", JsonUser(update_managed_bot_->bot_user_id_, client_));
  }

 private:
  const td_api::updateManagedBot *update_managed_bot_;
  const Client *client_;
};

class Client::JsonBotSubscriptionUpdated final : public td::Jsonable {
 public:
  JsonBotSubscriptionUpdated(const td_api::updateUserSubscription *update_user_subscription, const Client *client)
      : update_user_subscription_(update_user_subscription), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("user", JsonUser(update_user_subscription_->user_id_, client_));
    object("invoice_payload", update_user_subscription_->payload_);
    if (update_user_subscription_->is_canceled_) {
      object("state", "canceled");
    } else if (update_user_subscription_->is_restored_) {
      object("state", "active");
    } else if (update_user_subscription_->is_payment_failed_) {
      object("state", "failed");
    } else {
      UNREACHABLE();
    }
  }

 private:
  const td_api::updateUserSubscription *update_user_subscription_;
  const Client *client_;
};

class Client::JsonMessageGenerationStopped final : public td::Jsonable {
 public:
  JsonMessageGenerationStopped(const td_api::updateStopMessageDraft *update_stop_message_draft, const Client *client)
      : update_stop_message_draft_(update_stop_message_draft), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("chat", JsonChat(update_stop_message_draft_->chat_id_, client_));
    if (update_stop_message_draft_->forum_topic_id_ != 0) {
      object("message_thread_id", update_stop_message_draft_->forum_topic_id_);
    }
    object("draft_id", td::to_string(update_stop_message_draft_->draft_id_));
  }

 private:
  const td_api::updateStopMessageDraft *update_stop_message_draft_;
  const Client *client_;
};

class Client::JsonCommunityChatAdded final : public td::Jsonable {
 public:
  JsonCommunityChatAdded(const td_api::messageChatAddedToCommunity *chat_added_to_community, const Client *client)
      : chat_added_to_community_(chat_added_to_community), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("community", JsonCommunity(chat_added_to_community_->community_id_, client_));
  }

 private:
  const td_api::messageChatAddedToCommunity *chat_added_to_community_;
  const Client *client_;
};

class Client::JsonCommunityChatJoined final : public td::Jsonable {
 public:
  JsonCommunityChatJoined(const td_api::messageChatJoinFromCommunity *chat_join_from_community, const Client *client)
      : chat_join_from_community_(chat_join_from_community), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("community", JsonCommunity(chat_join_from_community_->community_id_, client_));
  }

 private:
  const td_api::messageChatJoinFromCommunity *chat_join_from_community_;
  const Client *client_;
};

class Client::JsonGiveawayCreated final : public td::Jsonable {
 public:
  explicit JsonGiveawayCreated(const td_api::messageGiveawayCreated *giveaway_created)
      : giveaway_created_(giveaway_created) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (giveaway_created_->star_count_ > 0) {
      object("prize_star_count", giveaway_created_->star_count_);
    }
  }

 private:
  const td_api::messageGiveawayCreated *giveaway_created_;
};

class Client::JsonGiveaway final : public td::Jsonable {
 public:
  JsonGiveaway(const td_api::messageGiveaway *giveaway, const Client *client) : giveaway_(giveaway), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    td::vector<int64> chat_ids;
    chat_ids.push_back(giveaway_->parameters_->boosted_chat_id_);
    for (auto chat_id : giveaway_->parameters_->additional_chat_ids_) {
      chat_ids.push_back(chat_id);
    }
    object("chats", td::json_array(chat_ids, [client = client_](auto &chat_id) { return JsonChat(chat_id, client); }));
    object("winners_selection_date", giveaway_->parameters_->winners_selection_date_);
    object("winner_count", giveaway_->winner_count_);
    if (giveaway_->parameters_->only_new_members_) {
      object("only_new_members", td::JsonTrue());
    }
    if (giveaway_->parameters_->has_public_winners_) {
      object("has_public_winners", td::JsonTrue());
    }
    if (!giveaway_->parameters_->country_codes_.empty()) {
      object("country_codes", td::json_array(giveaway_->parameters_->country_codes_,
                                             [](td::Slice country_code) { return td::JsonString(country_code); }));
    }
    if (!giveaway_->parameters_->prize_description_.empty()) {
      object("prize_description", giveaway_->parameters_->prize_description_);
    }
    switch (giveaway_->prize_->get_id()) {
      case td_api::giveawayPrizePremium::ID: {
        auto month_count = static_cast<const td_api::giveawayPrizePremium *>(giveaway_->prize_.get())->month_count_;
        if (month_count > 0) {
          object("premium_subscription_month_count", month_count);
        }
        break;
      }
      case td_api::giveawayPrizeStars::ID: {
        auto star_count = static_cast<const td_api::giveawayPrizeStars *>(giveaway_->prize_.get())->star_count_;
        if (star_count > 0) {
          object("prize_star_count", star_count);
        }
        break;
      }
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::messageGiveaway *giveaway_;
  const Client *client_;
};

class Client::JsonGiveawayWinners final : public td::Jsonable {
 public:
  JsonGiveawayWinners(const td_api::messageGiveawayWinners *giveaway_winners, const Client *client)
      : giveaway_winners_(giveaway_winners), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("chat", JsonChat(giveaway_winners_->boosted_chat_id_, client_));
    object("giveaway_message_id", as_client_message_id(giveaway_winners_->giveaway_message_id_));
    if (giveaway_winners_->additional_chat_count_ > 0) {
      object("additional_chat_count", giveaway_winners_->additional_chat_count_);
    }
    object("winners_selection_date", giveaway_winners_->actual_winners_selection_date_);
    if (giveaway_winners_->only_new_members_) {
      object("only_new_members", td::JsonTrue());
    }
    if (giveaway_winners_->was_refunded_) {
      object("was_refunded", td::JsonTrue());
    }
    switch (giveaway_winners_->prize_->get_id()) {
      case td_api::giveawayPrizePremium::ID: {
        auto month_count =
            static_cast<const td_api::giveawayPrizePremium *>(giveaway_winners_->prize_.get())->month_count_;
        if (month_count > 0) {
          object("premium_subscription_month_count", month_count);
        }
        break;
      }
      case td_api::giveawayPrizeStars::ID: {
        auto star_count = static_cast<const td_api::giveawayPrizeStars *>(giveaway_winners_->prize_.get())->star_count_;
        if (star_count > 0) {
          object("prize_star_count", star_count);
        }
        break;
      }
      default:
        UNREACHABLE();
    }
    if (!giveaway_winners_->prize_description_.empty()) {
      object("prize_description", giveaway_winners_->prize_description_);
    }
    object("winner_count", giveaway_winners_->winner_count_);
    if (giveaway_winners_->unclaimed_prize_count_ > 0) {
      object("unclaimed_prize_count", giveaway_winners_->unclaimed_prize_count_);
    }
    object("winners", JsonUsers(giveaway_winners_->winner_user_ids_, client_));
  }

 private:
  const td_api::messageGiveawayWinners *giveaway_winners_;
  const Client *client_;
};

class Client::JsonGiveawayCompleted final : public td::Jsonable {
 public:
  JsonGiveawayCompleted(const td_api::messageGiveawayCompleted *giveaway_completed, int64 chat_id, const Client *client)
      : giveaway_completed_(giveaway_completed), chat_id_(chat_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("winner_count", giveaway_completed_->winner_count_);
    if (giveaway_completed_->unclaimed_prize_count_ > 0) {
      object("unclaimed_prize_count", giveaway_completed_->unclaimed_prize_count_);
    }
    if (giveaway_completed_->is_star_giveaway_) {
      object("is_star_giveaway", td::JsonTrue());
    }
    const MessageInfo *giveaway_message =
        client_->get_message(chat_id_, giveaway_completed_->giveaway_message_id_, true);
    if (giveaway_message != nullptr) {
      object("giveaway_message", JsonMessage(giveaway_message, true, "giveaway completed", client_));
    }
  }

 private:
  const td_api::messageGiveawayCompleted *giveaway_completed_;
  int64 chat_id_;
  const Client *client_;
};

class Client::JsonChatBoostAdded final : public td::Jsonable {
 public:
  explicit JsonChatBoostAdded(const td_api::messageChatBoost *chat_boost) : chat_boost_(chat_boost) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("boost_count", chat_boost_->boost_count_);
  }

 private:
  const td_api::messageChatBoost *chat_boost_;
};

class Client::JsonPaidMessagePriceChanged final : public td::Jsonable {
 public:
  explicit JsonPaidMessagePriceChanged(const td_api::messagePaidMessagePriceChanged *price_changed)
      : price_changed_(price_changed) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("paid_message_star_count", price_changed_->paid_message_star_count_);
  }

 private:
  const td_api::messagePaidMessagePriceChanged *price_changed_;
};

class Client::JsonDirectMessagePriceChanged final : public td::Jsonable {
 public:
  explicit JsonDirectMessagePriceChanged(const td_api::messageDirectMessagePriceChanged *price_changed)
      : price_changed_(price_changed) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("are_direct_messages_enabled", td::JsonBool(price_changed_->is_enabled_));
    if (price_changed_->paid_message_star_count_ > 0) {
      object("direct_message_star_count", price_changed_->paid_message_star_count_);
    }
  }

 private:
  const td_api::messageDirectMessagePriceChanged *price_changed_;
};

class Client::JsonWebAppInfo final : public td::Jsonable {
 public:
  explicit JsonWebAppInfo(const td::string &url) : url_(url) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("url", url_);
  }

 private:
  const td::string &url_;
};

class Client::JsonCopyTextButton final : public td::Jsonable {
 public:
  explicit JsonCopyTextButton(const td::string &text) : text_(text) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("text", text_);
  }

 private:
  const td::string &text_;
};

class Client::JsonInlineKeyboardButton final : public td::Jsonable {
 public:
  explicit JsonInlineKeyboardButton(const td_api::inlineKeyboardButton *button) : button_(button) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("text", button_->text_);
    if (button_->icon_custom_emoji_id_ != 0) {
      object("icon_custom_emoji_id", td::to_string(button_->icon_custom_emoji_id_));
    }
    json_store_style(object, button_->style_.get(), false);
    json_store_inline_keyboard_button_type(object, button_->type_.get(), false);
  }

 private:
  const td_api::inlineKeyboardButton *button_;
};

class Client::JsonInlineKeyboard final : public td::Jsonable {
 public:
  explicit JsonInlineKeyboard(const td_api::replyMarkupInlineKeyboard *inline_keyboard)
      : inline_keyboard_(inline_keyboard) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &row : inline_keyboard_->rows_) {
      array << td::json_array(row, [](auto &button) { return JsonInlineKeyboardButton(button.get()); });
    }
  }

 private:
  const td_api::replyMarkupInlineKeyboard *inline_keyboard_;
};

class Client::JsonReplyMarkup final : public td::Jsonable {
 public:
  explicit JsonReplyMarkup(const td_api::ReplyMarkup *reply_markup) : reply_markup_(reply_markup) {
  }
  void store(td::JsonValueScope *scope) const {
    CHECK(reply_markup_->get_id() == td_api::replyMarkupInlineKeyboard::ID);
    auto inline_reply_markup = static_cast<const td_api::replyMarkupInlineKeyboard *>(reply_markup_);
    auto object = scope->enter_object();
    object("inline_keyboard", JsonInlineKeyboard(inline_reply_markup));
    if (inline_reply_markup->force_reply_) {
      object("force_reply", td::JsonTrue());
    }
  }

 private:
  const td_api::ReplyMarkup *reply_markup_;
};

class Client::JsonDirectMessagesTopic final : public td::Jsonable {
 public:
  JsonDirectMessagesTopic(int64 topic_id, const Client *client) : topic_id_(topic_id), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("topic_id", topic_id_);
    object("user", JsonUser(topic_id_, client_));
  }

 private:
  int64 topic_id_;
  const Client *client_;
};

class Client::JsonExternalReplyInfo final : public td::Jsonable {
 public:
  JsonExternalReplyInfo(const td_api::messageReplyToMessage *reply, const Client *client)
      : reply_(reply), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("origin", JsonMessageOrigin(reply_->origin_.get(), reply_->origin_send_date_, client_));
    if (reply_->chat_id_ != 0) {
      object("chat", JsonChat(reply_->chat_id_, client_));
      if (reply_->message_id_ != 0) {
        object("message_id", as_client_message_id(reply_->message_id_));
      }
    }
    if (reply_->content_ != nullptr) {
      switch (reply_->content_->get_id()) {
        case td_api::messageText::ID: {
          auto content = static_cast<const td_api::messageText *>(reply_->content_.get());
          if (content->link_preview_options_ != nullptr) {
            object("link_preview_options", JsonLinkPreviewOptions(content->link_preview_options_.get()));
          }
          break;
        }
        case td_api::messageAnimation::ID: {
          auto content = static_cast<const td_api::messageAnimation *>(reply_->content_.get());
          object("animation", JsonAnimation(content->animation_.get(), false, client_));
          add_media_spoiler(object, content->has_spoiler_);
          break;
        }
        case td_api::messageAudio::ID: {
          auto content = static_cast<const td_api::messageAudio *>(reply_->content_.get());
          object("audio", JsonAudio(content->audio_.get(), client_));
          break;
        }
        case td_api::messageDocument::ID: {
          auto content = static_cast<const td_api::messageDocument *>(reply_->content_.get());
          object("document", JsonDocument(content->document_.get(), client_));
          break;
        }
        case td_api::messagePaidMedia::ID: {
          auto content = static_cast<const td_api::messagePaidMedia *>(reply_->content_.get());
          object("paid_media", JsonPaidMediaInfo(content, client_));
          break;
        }
        case td_api::messagePhoto::ID: {
          auto content = static_cast<const td_api::messagePhoto *>(reply_->content_.get());
          CHECK(content->photo_ != nullptr);
          if (content->video_ != nullptr) {
            object("live_photo", JsonLivePhoto(content->photo_.get(), content->video_.get(), client_));
          } else {
            object("photo", JsonPhoto(content->photo_.get(), client_));
          }
          add_media_spoiler(object, content->has_spoiler_);
          break;
        }
        case td_api::messageSticker::ID: {
          auto content = static_cast<const td_api::messageSticker *>(reply_->content_.get());
          object("sticker", JsonSticker(content->sticker_.get(), client_));
          break;
        }
        case td_api::messageVideo::ID: {
          auto content = static_cast<const td_api::messageVideo *>(reply_->content_.get());
          object("video", JsonVideo(content->video_.get(), content->cover_.get(), content->start_timestamp_,
                                    &content->alternative_videos_, client_));
          add_media_spoiler(object, content->has_spoiler_);
          break;
        }
        case td_api::messageVideoNote::ID: {
          auto content = static_cast<const td_api::messageVideoNote *>(reply_->content_.get());
          object("video_note", JsonVideoNote(content->video_note_.get(), client_));
          break;
        }
        case td_api::messageVoiceNote::ID: {
          auto content = static_cast<const td_api::messageVoiceNote *>(reply_->content_.get());
          object("voice", JsonVoiceNote(content->voice_note_.get(), client_));
          break;
        }
        case td_api::messageContact::ID: {
          auto content = static_cast<const td_api::messageContact *>(reply_->content_.get());
          object("contact", JsonContact(content->contact_.get()));
          break;
        }
        case td_api::messageDice::ID: {
          auto content = static_cast<const td_api::messageDice *>(reply_->content_.get());
          object("dice", JsonDice(content->emoji_, content->value_));
          break;
        }
        case td_api::messageGame::ID: {
          auto content = static_cast<const td_api::messageGame *>(reply_->content_.get());
          object("game", JsonGame(content->game_.get(), client_));
          break;
        }
        case td_api::messageInvoice::ID: {
          auto content = static_cast<const td_api::messageInvoice *>(reply_->content_.get());
          object("invoice", JsonInvoice(content));
          break;
        }
        case td_api::messageLiveLocation::ID: {
          auto content = static_cast<const td_api::messageLiveLocation *>(reply_->content_.get());
          object("location", JsonLiveLocation(content->location_.get(), content->expires_in_));
          break;
        }
        case td_api::messageLocation::ID: {
          auto content = static_cast<const td_api::messageLocation *>(reply_->content_.get());
          object("location", JsonLocation(content->location_.get()));
          break;
        }
        case td_api::messageStakeDice::ID: {
          auto content = static_cast<const td_api::messageStakeDice *>(reply_->content_.get());
          object("dice", JsonDice("🎲", content->value_));
          break;
        }
        case td_api::messageVenue::ID: {
          auto content = static_cast<const td_api::messageVenue *>(reply_->content_.get());
          object("venue", JsonVenue(content->venue_.get()));
          break;
        }
        case td_api::messagePoll::ID: {
          auto content = static_cast<const td_api::messagePoll *>(reply_->content_.get());
          object("poll", JsonPoll(content->poll_.get(), content->description_.get(), content->media_.get(), client_));
          break;
        }
        case td_api::messageUnsupported::ID:
          break;
        case td_api::messageGiveaway::ID: {
          auto content = static_cast<const td_api::messageGiveaway *>(reply_->content_.get());
          object("giveaway", JsonGiveaway(content, client_));
          break;
        }
        case td_api::messageGiveawayWinners::ID: {
          auto content = static_cast<const td_api::messageGiveawayWinners *>(reply_->content_.get());
          object("giveaway_winners", JsonGiveawayWinners(content, client_));
          break;
        }
        case td_api::messageStory::ID: {
          auto content = static_cast<const td_api::messageStory *>(reply_->content_.get());
          object("story", JsonStory(content->story_poster_chat_id_, content->story_id_, client_));
          break;
        }
        case td_api::messageChecklist::ID: {
          auto content = static_cast<const td_api::messageChecklist *>(reply_->content_.get());
          object("checklist", JsonChecklist(content->list_.get(), client_));
          break;
        }
        default:
          LOG(ERROR) << "Receive external reply with " << to_string(reply_->content_);
      }
    }
  }

 private:
  const td_api::messageReplyToMessage *reply_;
  const Client *client_;

  void add_media_spoiler(td::JsonObjectScope &object, bool has_spoiler) const {
    if (has_spoiler) {
      object("has_media_spoiler", td::JsonTrue());
    }
  }
};

class Client::JsonTextQuote final : public td::Jsonable {
 public:
  JsonTextQuote(const td_api::textQuote *quote, const Client *client) : quote_(quote), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("text", quote_->text_->text_);
    if (!quote_->text_->entities_.empty()) {
      object("entities", JsonVectorEntities(quote_->text_->entities_, client_));
    }
    object("position", quote_->position_);
    if (quote_->is_manual_) {
      object("is_manual", td::JsonTrue());
    }
  }

 private:
  const td_api::textQuote *quote_;
  const Client *client_;
};

void Client::JsonRichBlock::store(td::JsonValueScope *scope) const {
  auto object = scope->enter_object();
  switch (block_->get_id()) {
    case td_api::pageBlockSectionHeading::ID: {
      const auto *block = static_cast<const td_api::pageBlockSectionHeading *>(block_);
      object("type", "heading");
      object("text", JsonRichText(block->text_.get(), client_));
      object("size", block->size_);
      return;
    }
    case td_api::pageBlockParagraph::ID: {
      const auto *block = static_cast<const td_api::pageBlockParagraph *>(block_);
      object("type", "paragraph");
      object("text", JsonRichText(block->text_.get(), client_));
      return;
    }
    case td_api::pageBlockPreformatted::ID: {
      const auto *block = static_cast<const td_api::pageBlockPreformatted *>(block_);
      object("type", "pre");
      object("text", JsonRichText(block->text_.get(), client_));
      if (!block->language_.empty()) {
        object("language", block->language_);
      }
      return;
    }
    case td_api::pageBlockFooter::ID: {
      const auto *block = static_cast<const td_api::pageBlockFooter *>(block_);
      object("type", "footer");
      object("text", JsonRichText(block->footer_.get(), client_));
      return;
    }
    case td_api::pageBlockDivider::ID:
      object("type", "divider");
      return;
    case td_api::pageBlockMathematicalExpression::ID: {
      const auto *block = static_cast<const td_api::pageBlockMathematicalExpression *>(block_);
      object("type", "mathematical_expression");
      object("expression", block->expression_);
      return;
    }
    case td_api::pageBlockAnchor::ID: {
      const auto *block = static_cast<const td_api::pageBlockAnchor *>(block_);
      object("type", "anchor");
      object("name", block->name_);
      return;
    }
    case td_api::pageBlockList::ID: {
      const auto *block = static_cast<const td_api::pageBlockList *>(block_);
      object("type", "list");
      object("items", td::json_array(block->items_, [client = client_](const auto &item) {
               return JsonRichBlockListItem(item.get(), client);
             }));
      return;
    }
    case td_api::pageBlockBlockQuote::ID: {
      const auto *block = static_cast<const td_api::pageBlockBlockQuote *>(block_);
      object("type", "blockquote");
      object("blocks", JsonRichBlocks(block->blocks_, client_));
      if (block->credit_ != nullptr) {
        object("credit", JsonRichText(block->credit_.get(), client_));
      }
      return;
    }
    case td_api::pageBlockExpandableBlockQuote::ID: {
      const auto *block = static_cast<const td_api::pageBlockExpandableBlockQuote *>(block_);
      object("type", "expandable_blockquote");
      object("text", JsonRichText(block->text_.get(), client_));
      if (block->credit_ != nullptr) {
        object("credit", JsonRichText(block->credit_.get(), client_));
      }
      return;
    }
    case td_api::pageBlockPullQuote::ID: {
      const auto *block = static_cast<const td_api::pageBlockPullQuote *>(block_);
      object("type", "pullquote");
      object("text", JsonRichText(block->text_.get(), client_));
      if (block->credit_ != nullptr) {
        object("credit", JsonRichText(block->credit_.get(), client_));
      }
      return;
    }
    case td_api::pageBlockCollage::ID: {
      const auto *block = static_cast<const td_api::pageBlockCollage *>(block_);
      object("type", "collage");
      object("blocks", JsonRichBlocks(block->blocks_, client_));
      if (block->caption_ != nullptr) {
        object("caption", JsonRichBlockCaption(block->caption_.get(), client_));
      }
      return;
    }
    case td_api::pageBlockSlideshow::ID: {
      const auto *block = static_cast<const td_api::pageBlockSlideshow *>(block_);
      object("type", "slideshow");
      object("blocks", JsonRichBlocks(block->blocks_, client_));
      if (block->caption_ != nullptr) {
        object("caption", JsonRichBlockCaption(block->caption_.get(), client_));
      }
      return;
    }
    case td_api::pageBlockTable::ID: {
      const auto *block = static_cast<const td_api::pageBlockTable *>(block_);
      object("type", "table");
      object("cells", td::json_array(block->cells_,
                                     [client = client_](const auto &row) { return JsonRichTableRow(row, client); }));
      if (block->caption_ != nullptr) {
        object("caption", JsonRichText(block->caption_.get(), client_));
      }
      if (block->is_bordered_) {
        object("is_bordered", td::JsonTrue());
      }
      if (block->is_striped_) {
        object("is_striped", td::JsonTrue());
      }
      if (block->is_compact_) {
        object("is_compact", td::JsonTrue());
      }
      return;
    }
    case td_api::pageBlockDetails::ID: {
      const auto *block = static_cast<const td_api::pageBlockDetails *>(block_);
      object("type", "details");
      object("summary", JsonRichText(block->header_.get(), client_));
      object("blocks", JsonRichBlocks(block->blocks_, client_));
      if (block->is_open_) {
        object("is_open", td::JsonTrue());
      }
      return;
    }
    case td_api::pageBlockMap::ID: {
      const auto *block = static_cast<const td_api::pageBlockMap *>(block_);
      object("type", "map");
      object("location", JsonLocation(block->location_.get()));
      object("zoom", block->zoom_);
      object("width", block->width_);
      object("height", block->height_);
      if (block->caption_ != nullptr) {
        object("caption", JsonRichBlockCaption(block->caption_.get(), client_));
      }
      return;
    }
    case td_api::pageBlockButtonRow::ID: {
      const auto *block = static_cast<const td_api::pageBlockButtonRow *>(block_);
      object("type", "buttons");
      object("buttons", td::json_array(block->buttons_, [client = client_](auto &button) {
               return JsonRichMessageButton(button.get(), client);
             }));
      if (block->align_ != nullptr) {
        json_store_horizontal_alignment(object, block->align_.get());
      }
      return;
    }
    case td_api::pageBlockAnimation::ID: {
      const auto *block = static_cast<const td_api::pageBlockAnimation *>(block_);
      if (block->animation_ == nullptr) {
        break;
      }
      object("type", "animation");
      object("animation", JsonAnimation(block->animation_.get(), false, client_));
      if (block->caption_ != nullptr) {
        object("caption", JsonRichBlockCaption(block->caption_.get(), client_));
      }
      if (block->need_autoplay_) {
        object("need_autoplay", td::JsonTrue());
      }
      if (block->has_spoiler_) {
        object("has_spoiler", td::JsonTrue());
      }
      return;
    }
    case td_api::pageBlockAudio::ID: {
      const auto *block = static_cast<const td_api::pageBlockAudio *>(block_);
      object("type", "audio");
      object("audio", JsonAudio(block->audio_.get(), client_));
      if (block->caption_ != nullptr) {
        object("caption", JsonRichBlockCaption(block->caption_.get(), client_));
      }
      return;
    }
    case td_api::pageBlockDocument::ID: {
      const auto *block = static_cast<const td_api::pageBlockDocument *>(block_);
      object("type", "document");
      object("audio", JsonDocument(block->document_.get(), client_));
      if (block->caption_ != nullptr) {
        object("caption", JsonRichBlockCaption(block->caption_.get(), client_));
      }
      return;
    }
    case td_api::pageBlockPhoto::ID: {
      const auto *block = static_cast<const td_api::pageBlockPhoto *>(block_);
      if (block->photo_ == nullptr) {
        break;
      }
      object("type", "photo");
      object("photo", JsonPhoto(block->photo_.get(), client_));
      if (block->caption_ != nullptr) {
        object("caption", JsonRichBlockCaption(block->caption_.get(), client_));
      }
      if (block->has_spoiler_) {
        object("has_spoiler", td::JsonTrue());
      }
      return;
    }
    case td_api::pageBlockVideo::ID: {
      const auto *block = static_cast<const td_api::pageBlockVideo *>(block_);
      if (block->video_ == nullptr) {
        break;
      }
      object("type", "video");
      object("video", JsonVideo(block->video_.get(), nullptr, 0, nullptr, client_));
      if (block->caption_ != nullptr) {
        object("caption", JsonRichBlockCaption(block->caption_.get(), client_));
      }
      if (block->need_autoplay_) {
        object("need_autoplay", td::JsonTrue());
      }
      if (block->is_looped_) {
        object("is_looped", td::JsonTrue());
      }
      if (block->has_spoiler_) {
        object("has_spoiler", td::JsonTrue());
      }
      return;
    }
    case td_api::pageBlockVoiceNote::ID: {
      const auto *block = static_cast<const td_api::pageBlockVoiceNote *>(block_);
      object("type", "voice_note");
      object("voice_note", JsonVoiceNote(block->voice_note_.get(), client_));
      if (block->caption_ != nullptr) {
        object("caption", JsonRichBlockCaption(block->caption_.get(), client_));
      }
      return;
    }
    case td_api::pageBlockUnsupported::ID:
      object("type", "unsupported");
      return;
    default:
      break;
  }
  LOG(ERROR) << "Receive " << td_api::to_string(*block_);
  object("type", "unknown");
}

void Client::JsonMessage::store(td::JsonValueScope *scope) const {
  CHECK(message_ != nullptr);
  auto object = scope->enter_object();
  if (!message_->business_connection_id.empty()) {
    object("business_connection_id", message_->business_connection_id);
    if (message_->sender_business_bot_user_id != 0) {
      object("sender_business_bot", JsonUser(message_->sender_business_bot_user_id, client_));
    }
  }
  object("message_id", as_client_message_id_unchecked(message_->id));
  if (message_->ephemeral_message_id != 0) {
    object("ephemeral_message_id", message_->ephemeral_message_id);
  }
  if (message_->sender_user_id != 0) {
    object("from", JsonUser(message_->sender_user_id, client_));
  }
  if (!message_->author_signature.empty()) {
    object("author_signature", message_->author_signature);
  }
  if (message_->sender_chat_id != 0) {
    object("sender_chat", JsonChat(message_->sender_chat_id, client_));
  }
  object("chat", JsonChat(message_->chat_id, client_));
  object("date", message_->date);
  if (message_->edit_date > 0) {
    object("edit_date", message_->edit_date);
  }
  auto forum_topic_id = get_forum_topic_id(message_->topic_id);
  if (forum_topic_id != 0) {
    object("message_thread_id", forum_topic_id);
  }
  if (message_->receiver_id != nullptr) {
    client_->json_store_message_sender(object, message_->receiver_id, "receiver_user", "receiver_chat");
  }
  if (message_->initial_send_date > 0) {
    CHECK(message_->forward_origin != nullptr);
    object("forward_origin", JsonMessageOrigin(message_->forward_origin.get(), message_->initial_send_date, client_));
    if (message_->is_automatic_forward) {
      object("is_automatic_forward", td::JsonTrue());
    }

    switch (message_->forward_origin->get_id()) {
      case td_api::messageOriginUser::ID: {
        auto forward_info = static_cast<const td_api::messageOriginUser *>(message_->forward_origin.get());
        object("forward_from", JsonUser(forward_info->sender_user_id_, client_));
        break;
      }
      case td_api::messageOriginChat::ID: {
        auto forward_info = static_cast<const td_api::messageOriginChat *>(message_->forward_origin.get());
        object("forward_from_chat", JsonChat(forward_info->sender_chat_id_, client_));
        if (!forward_info->author_signature_.empty()) {
          object("forward_signature", forward_info->author_signature_);
        }
        break;
      }
      case td_api::messageOriginHiddenUser::ID: {
        auto forward_info = static_cast<const td_api::messageOriginHiddenUser *>(message_->forward_origin.get());
        if (!forward_info->sender_name_.empty()) {
          object("forward_sender_name", forward_info->sender_name_);
        }
        break;
      }
      case td_api::messageOriginChannel::ID: {
        auto forward_info = static_cast<const td_api::messageOriginChannel *>(message_->forward_origin.get());
        object("forward_from_chat", JsonChat(forward_info->chat_id_, client_));
        object("forward_from_message_id", as_client_message_id(forward_info->message_id_));
        if (!forward_info->author_signature_.empty()) {
          object("forward_signature", forward_info->author_signature_);
        }
        break;
      }
      default:
        UNREACHABLE();
    }
    object("forward_date", message_->initial_send_date);
  }
  if (need_reply_) {
    auto reply_to_message_id = client_->get_same_chat_reply_to_message_id(message_, true);
    if (reply_to_message_id > 0) {
      // internal reply
      const auto *reply_to_message = client_->get_reply_to_message_info(message_, reply_to_message_id);
      if (reply_to_message != nullptr) {
        object("reply_to_message", JsonMessage(reply_to_message, false, "reply in " + source_, client_));
      } else {
        LOG(INFO) << "Replied to unknown or deleted message " << reply_to_message_id << " in chat " << message_->chat_id
                  << " while storing " << source_ << ' ' << message_->id;
      }
    }
  }
  if (message_->reply_to_message != nullptr && message_->reply_to_message->origin_ != nullptr) {
    object("external_reply", JsonExternalReplyInfo(message_->reply_to_message.get(), client_));
  }
  if (message_->reply_to_message != nullptr && message_->reply_to_message->quote_ != nullptr) {
    object("quote", JsonTextQuote(message_->reply_to_message->quote_.get(), client_));
  }
  if (message_->reply_to_message != nullptr && message_->reply_to_message->checklist_task_id_ != 0) {
    object("reply_to_checklist_task_id", message_->reply_to_message->checklist_task_id_);
  }
  if (message_->reply_to_message != nullptr && !message_->reply_to_message->poll_option_id_.empty()) {
    object("reply_to_poll_option_id", message_->reply_to_message->poll_option_id_);
  }
  if (message_->reply_to_story != nullptr) {
    object("reply_to_story",
           JsonStory(message_->reply_to_story->story_poster_chat_id_, message_->reply_to_story->story_id_, client_));
  }
  if (message_->media_album_id != 0) {
    object("media_group_id", td::to_string(message_->media_album_id));
  }
  if (message_->guest_query_id != 0) {
    object("guest_query_id", td::to_string(message_->guest_query_id));
  }
  switch (message_->content->get_id()) {
    case td_api::messageText::ID: {
      auto content = static_cast<const td_api::messageText *>(message_->content.get());
      object("text", content->text_->text_);
      if (!content->text_->entities_.empty()) {
        object("entities", JsonVectorEntities(content->text_->entities_, client_));
      }
      if (content->link_preview_options_ != nullptr) {
        object("link_preview_options", JsonLinkPreviewOptions(content->link_preview_options_.get()));
      }
      break;
    }
    case td_api::messageAnimation::ID: {
      auto content = static_cast<const td_api::messageAnimation *>(message_->content.get());
      object("animation", JsonAnimation(content->animation_.get(), false, client_));
      object("document", JsonAnimation(content->animation_.get(), true, client_));
      add_caption(object, content->caption_, content->show_caption_above_media_);
      add_media_spoiler(object, content->has_spoiler_);
      break;
    }
    case td_api::messageAudio::ID: {
      auto content = static_cast<const td_api::messageAudio *>(message_->content.get());
      object("audio", JsonAudio(content->audio_.get(), client_));
      add_caption(object, content->caption_, false);
      break;
    }
    case td_api::messageDocument::ID: {
      auto content = static_cast<const td_api::messageDocument *>(message_->content.get());
      object("document", JsonDocument(content->document_.get(), client_));
      add_caption(object, content->caption_, false);
      break;
    }
    case td_api::messagePaidMedia::ID: {
      auto content = static_cast<const td_api::messagePaidMedia *>(message_->content.get());
      object("paid_media", JsonPaidMediaInfo(content, client_));
      add_caption(object, content->caption_, content->show_caption_above_media_);
      break;
    }
    case td_api::messagePhoto::ID: {
      auto content = static_cast<const td_api::messagePhoto *>(message_->content.get());
      CHECK(content->photo_ != nullptr);
      if (content->video_ != nullptr) {
        object("live_photo", JsonLivePhoto(content->photo_.get(), content->video_.get(), client_));
      }
      object("photo", JsonPhoto(content->photo_.get(), client_));
      add_caption(object, content->caption_, content->show_caption_above_media_);
      add_media_spoiler(object, content->has_spoiler_);
      break;
    }
    case td_api::messageSticker::ID: {
      auto content = static_cast<const td_api::messageSticker *>(message_->content.get());
      object("sticker", JsonSticker(content->sticker_.get(), client_));
      break;
    }
    case td_api::messageVideo::ID: {
      auto content = static_cast<const td_api::messageVideo *>(message_->content.get());
      object("video", JsonVideo(content->video_.get(), content->cover_.get(), content->start_timestamp_,
                                &content->alternative_videos_, client_));
      add_caption(object, content->caption_, content->show_caption_above_media_);
      add_media_spoiler(object, content->has_spoiler_);
      break;
    }
    case td_api::messageVideoNote::ID: {
      auto content = static_cast<const td_api::messageVideoNote *>(message_->content.get());
      object("video_note", JsonVideoNote(content->video_note_.get(), client_));
      break;
    }
    case td_api::messageVoiceNote::ID: {
      auto content = static_cast<const td_api::messageVoiceNote *>(message_->content.get());
      object("voice", JsonVoiceNote(content->voice_note_.get(), client_));
      add_caption(object, content->caption_, false);
      break;
    }
    case td_api::messageContact::ID: {
      auto content = static_cast<const td_api::messageContact *>(message_->content.get());
      object("contact", JsonContact(content->contact_.get()));
      break;
    }
    case td_api::messageDice::ID: {
      auto content = static_cast<const td_api::messageDice *>(message_->content.get());
      object("dice", JsonDice(content->emoji_, content->value_));
      break;
    }
    case td_api::messageGame::ID: {
      auto content = static_cast<const td_api::messageGame *>(message_->content.get());
      object("game", JsonGame(content->game_.get(), client_));
      break;
    }
    case td_api::messageInvoice::ID: {
      auto content = static_cast<const td_api::messageInvoice *>(message_->content.get());
      object("invoice", JsonInvoice(content));
      break;
    }
    case td_api::messageLiveLocation::ID: {
      auto content = static_cast<const td_api::messageLiveLocation *>(message_->content.get());
      object("location", JsonLiveLocation(content->location_.get(), content->expires_in_));
      break;
    }
    case td_api::messageLocation::ID: {
      auto content = static_cast<const td_api::messageLocation *>(message_->content.get());
      object("location", JsonLocation(content->location_.get()));
      break;
    }
    case td_api::messageStakeDice::ID: {
      auto content = static_cast<const td_api::messageStakeDice *>(message_->content.get());
      object("dice", JsonDice("🎲", content->value_));
      break;
    }
    case td_api::messageVenue::ID: {
      auto content = static_cast<const td_api::messageVenue *>(message_->content.get());
      object("location", JsonLocation(content->venue_->location_.get()));
      object("venue", JsonVenue(content->venue_.get()));
      break;
    }
    case td_api::messagePoll::ID: {
      auto content = static_cast<const td_api::messagePoll *>(message_->content.get());
      object("poll", JsonPoll(content->poll_.get(), content->description_.get(), content->media_.get(), client_));
      break;
    }
    case td_api::messageChatAddMembers::ID: {
      auto content = static_cast<const td_api::messageChatAddMembers *>(message_->content.get());
      int64 user_id = client_->choose_added_member_id(content);
      if (user_id > 0) {
        object("new_chat_participant", JsonUser(user_id, client_));
        object("new_chat_member", JsonUser(user_id, client_));
        object("new_chat_members", JsonUsers(content->member_user_ids_, client_));
      } else {
        LOG(ERROR) << "Can't choose added member for new_chat_member field";
      }
      break;
    }
    case td_api::messageChatJoinByLink::ID: {
      if (message_->sender_user_id > 0) {
        object("new_chat_participant", JsonUser(message_->sender_user_id, client_));
        object("new_chat_member", JsonUser(message_->sender_user_id, client_));
        object("new_chat_members", JsonUsers({message_->sender_user_id}, client_));
      }
      break;
    }
    case td_api::messageChatJoinByRequest::ID: {
      if (message_->sender_user_id > 0) {
        object("new_chat_participant", JsonUser(message_->sender_user_id, client_));
        object("new_chat_member", JsonUser(message_->sender_user_id, client_));
        object("new_chat_members", JsonUsers({message_->sender_user_id}, client_));
      }
      break;
    }
    case td_api::messageChatDeleteMember::ID: {
      auto content = static_cast<const td_api::messageChatDeleteMember *>(message_->content.get());
      int64 user_id = content->user_id_;
      object("left_chat_participant", JsonUser(user_id, client_));
      object("left_chat_member", JsonUser(user_id, client_));
      break;
    }
    case td_api::messageChatChangeTitle::ID: {
      auto content = static_cast<const td_api::messageChatChangeTitle *>(message_->content.get());
      object("new_chat_title", content->title_);
      break;
    }
    case td_api::messageChatChangePhoto::ID: {
      auto content = static_cast<const td_api::messageChatChangePhoto *>(message_->content.get());
      CHECK(content->photo_ != nullptr);
      object("new_chat_photo", JsonChatPhoto(content->photo_.get(), client_));
      break;
    }
    case td_api::messageChatDeletePhoto::ID:
      object("delete_chat_photo", td::JsonTrue());
      break;
    case td_api::messageBasicGroupChatCreate::ID:
      object("group_chat_created", td::JsonTrue());
      break;
    case td_api::messageSupergroupChatCreate::ID: {
      auto chat = client_->get_chat(message_->chat_id);
      if (chat->type != ChatInfo::Type::Supergroup) {
        LOG(ERROR) << "Receive messageSupergroupChatCreate in the non-supergroup chat " << message_->chat_id;
        break;
      }
      auto supergroup_info = client_->get_supergroup_info(chat->supergroup_id);
      CHECK(supergroup_info != nullptr);
      if (supergroup_info->is_supergroup) {
        object("supergroup_chat_created", td::JsonTrue());
      } else {
        object("channel_chat_created", td::JsonTrue());
      }
      break;
    }
    case td_api::messageChatUpgradeTo::ID: {
      auto content = static_cast<const td_api::messageChatUpgradeTo *>(message_->content.get());
      auto chat_id = get_supergroup_chat_id(content->supergroup_id_);
      object("migrate_to_chat_id", td::JsonLong(chat_id));
      break;
    }
    case td_api::messageChatUpgradeFrom::ID: {
      auto content = static_cast<const td_api::messageChatUpgradeFrom *>(message_->content.get());
      auto chat_id = get_basic_group_chat_id(content->basic_group_id_);
      object("migrate_from_chat_id", td::JsonLong(chat_id));
      break;
    }
    case td_api::messageForumTopicCreated::ID: {
      auto content = static_cast<const td_api::messageForumTopicCreated *>(message_->content.get());
      object("forum_topic_created", JsonForumTopicCreated(content));
      break;
    }
    case td_api::messageForumTopicEdited::ID: {
      auto content = static_cast<const td_api::messageForumTopicEdited *>(message_->content.get());
      object("forum_topic_edited", JsonForumTopicEdited(content));
      break;
    }
    case td_api::messageForumTopicIsClosedToggled::ID: {
      auto content = static_cast<const td_api::messageForumTopicIsClosedToggled *>(message_->content.get());
      if (content->is_closed_) {
        object("forum_topic_closed", JsonEmptyObject());
      } else {
        object("forum_topic_reopened", JsonEmptyObject());
      }
      break;
    }
    case td_api::messageForumTopicIsHiddenToggled::ID: {
      auto content = static_cast<const td_api::messageForumTopicIsHiddenToggled *>(message_->content.get());
      if (content->is_hidden_) {
        object("general_forum_topic_hidden", JsonEmptyObject());
      } else {
        object("general_forum_topic_unhidden", JsonEmptyObject());
      }
      break;
    }
    case td_api::messagePinMessage::ID: {
      auto content = static_cast<const td_api::messagePinMessage *>(message_->content.get());
      auto message_id = content->message_id_;
      if (message_id > 0) {
        const auto *pinned_message = client_->get_reply_to_message_info(message_, message_id);
        if (pinned_message != nullptr) {
          object("pinned_message", JsonMessage(pinned_message, false, "pin in " + source_, client_));
        } else if (need_reply_) {
          LOG(INFO) << "Pinned unknown, inaccessible or deleted message " << message_id;
          object("pinned_message", JsonInaccessibleMessage(message_->chat_id, message_id, client_));
        }
      }
      break;
    }
    case td_api::messageGameScore::ID:
      break;
    case td_api::messagePaymentSuccessful::ID:
      break;
    case td_api::messagePaymentSuccessfulBot::ID: {
      auto content = static_cast<const td_api::messagePaymentSuccessfulBot *>(message_->content.get());
      object("successful_payment", JsonSuccessfulPaymentBot(content));
      break;
    }
    case td_api::messageCall::ID:
      break;
    case td_api::messageScreenshotTaken::ID:
      break;
    case td_api::messageChatSetMessageAutoDeleteTime::ID: {
      auto content = static_cast<const td_api::messageChatSetMessageAutoDeleteTime *>(message_->content.get());
      object("message_auto_delete_timer_changed", JsonChatSetMessageAutoDeleteTime(content));
      break;
    }
    case td_api::messageUnsupported::ID:
      break;
    case td_api::messageContactRegistered::ID:
      break;
    case td_api::messageExpiredPhoto::ID:
      break;
    case td_api::messageExpiredVideo::ID:
      break;
    case td_api::messageExpiredVideoNote::ID:
      break;
    case td_api::messageExpiredVoiceNote::ID:
      break;
    case td_api::messageCustomServiceAction::ID:
      break;
    case td_api::messageChatSetTheme::ID:
      break;
    case td_api::messageAnimatedEmoji::ID:
      UNREACHABLE();
      break;
    case td_api::messagePassportDataSent::ID:
      break;
    case td_api::messagePassportDataReceived::ID: {
      auto content = static_cast<const td_api::messagePassportDataReceived *>(message_->content.get());
      object("passport_data", JsonPassportData(content, client_));
      break;
    }
    case td_api::messageProximityAlertTriggered::ID: {
      auto content = static_cast<const td_api::messageProximityAlertTriggered *>(message_->content.get());
      object("proximity_alert_triggered", JsonProximityAlertTriggered(content, client_));
      break;
    }
    case td_api::messageVideoChatScheduled::ID: {
      auto content = static_cast<const td_api::messageVideoChatScheduled *>(message_->content.get());
      object("video_chat_scheduled", JsonVideoChatScheduled(content));
      object("voice_chat_scheduled", JsonVideoChatScheduled(content));
      break;
    }
    case td_api::messageVideoChatStarted::ID:
      object("video_chat_started", JsonEmptyObject());
      object("voice_chat_started", JsonEmptyObject());
      break;
    case td_api::messageVideoChatEnded::ID: {
      auto content = static_cast<const td_api::messageVideoChatEnded *>(message_->content.get());
      object("video_chat_ended", JsonVideoChatEnded(content));
      object("voice_chat_ended", JsonVideoChatEnded(content));
      break;
    }
    case td_api::messageInviteVideoChatParticipants::ID: {
      auto content = static_cast<const td_api::messageInviteVideoChatParticipants *>(message_->content.get());
      object("video_chat_participants_invited", JsonInviteVideoChatParticipants(content, client_));
      object("voice_chat_participants_invited", JsonInviteVideoChatParticipants(content, client_));
      break;
    }
    case td_api::messageWebAppDataSent::ID:
      break;
    case td_api::messageWebAppDataReceived::ID: {
      auto content = static_cast<const td_api::messageWebAppDataReceived *>(message_->content.get());
      object("web_app_data", JsonWebAppData(content));
      break;
    }
    case td_api::messageGiftedPremium::ID:
      break;
    case td_api::messageSuggestProfilePhoto::ID:
      break;
    case td_api::messageBotWriteAccessAllowed::ID: {
      auto chat = client_->get_chat(message_->chat_id);
      if (chat->type != ChatInfo::Type::Private) {
        break;
      }

      auto content = static_cast<const td_api::messageBotWriteAccessAllowed *>(message_->content.get());
      if (content->reason_->get_id() == td_api::botWriteAccessAllowReasonConnectedWebsite::ID) {
        auto reason = static_cast<const td_api::botWriteAccessAllowReasonConnectedWebsite *>(content->reason_.get());
        if (!reason->domain_name_.empty()) {
          object("connected_website", reason->domain_name_);
        }
      } else {
        object("write_access_allowed", JsonWriteAccessAllowed(content));
      }
      break;
    }
    case td_api::messageUsersShared::ID: {
      auto content = static_cast<const td_api::messageUsersShared *>(message_->content.get());
      if (content->users_.size() == 1) {
        object("user_shared", JsonUserShared(content));
      }
      object("users_shared", JsonUsersShared(content, client_));
      break;
    }
    case td_api::messageChatShared::ID: {
      auto content = static_cast<const td_api::messageChatShared *>(message_->content.get());
      object("chat_shared", JsonChatShared(content, client_));
      break;
    }
    case td_api::messageStory::ID: {
      auto content = static_cast<const td_api::messageStory *>(message_->content.get());
      object("story", JsonStory(content->story_poster_chat_id_, content->story_id_, client_));
      break;
    }
    case td_api::messageChatSetBackground::ID: {
      auto content = static_cast<const td_api::messageChatSetBackground *>(message_->content.get());
      object("chat_background_set", JsonChatBackground(content->background_.get(), client_));
      break;
    }
    case td_api::messagePremiumGiftCode::ID:
      break;
    case td_api::messageGiveawayCreated::ID: {
      auto content = static_cast<const td_api::messageGiveawayCreated *>(message_->content.get());
      object("giveaway_created", JsonGiveawayCreated(content));
      break;
    }
    case td_api::messageGiveaway::ID: {
      auto content = static_cast<const td_api::messageGiveaway *>(message_->content.get());
      object("giveaway", JsonGiveaway(content, client_));
      break;
    }
    case td_api::messageGiveawayWinners::ID: {
      auto content = static_cast<const td_api::messageGiveawayWinners *>(message_->content.get());
      object("giveaway_winners", JsonGiveawayWinners(content, client_));
      break;
    }
    case td_api::messageGiveawayCompleted::ID: {
      auto content = static_cast<const td_api::messageGiveawayCompleted *>(message_->content.get());
      object("giveaway_completed", JsonGiveawayCompleted(content, message_->chat_id, client_));
      break;
    }
    case td_api::messageChatBoost::ID: {
      auto content = static_cast<const td_api::messageChatBoost *>(message_->content.get());
      object("boost_added", JsonChatBoostAdded(content));
      break;
    }
    case td_api::messagePaymentRefunded::ID: {
      auto content = static_cast<const td_api::messagePaymentRefunded *>(message_->content.get());
      object("refunded_payment", JsonRefundedPayment(content));
      break;
    }
    case td_api::messageGiftedStars::ID:
      break;
    case td_api::messageGiveawayPrizeStars::ID:
      break;
    case td_api::messageGift::ID: {
      auto content = static_cast<const td_api::messageGift *>(message_->content.get());
      if (content->is_prepaid_upgrade_) {
        object("gift_upgrade_sent", JsonGiftMessage(content, client_));
      } else {
        object("gift", JsonGiftMessage(content, client_));
      }
      break;
    }
    case td_api::messageUpgradedGift::ID: {
      auto content = static_cast<const td_api::messageUpgradedGift *>(message_->content.get());
      object("unique_gift", JsonUniqueGiftMessage(content, client_));
      break;
    }
    case td_api::messageRefundedUpgradedGift::ID: {
      auto content = static_cast<const td_api::messageRefundedUpgradedGift *>(message_->content.get());
      object("gift", JsonRefundedUpgradedGiftMessage(content, client_));
      break;
    }
    case td_api::messagePaidMessagesRefunded::ID:
      break;
    case td_api::messagePaidMessagePriceChanged::ID: {
      auto content = static_cast<const td_api::messagePaidMessagePriceChanged *>(message_->content.get());
      object("paid_message_price_changed", JsonPaidMessagePriceChanged(content));
      break;
    }
    case td_api::messageGroupCall::ID:
      break;
    case td_api::messageDirectMessagePriceChanged::ID: {
      auto content = static_cast<const td_api::messageDirectMessagePriceChanged *>(message_->content.get());
      object("direct_message_price_changed", JsonDirectMessagePriceChanged(content));
      break;
    }
    case td_api::messageChecklist::ID: {
      auto content = static_cast<const td_api::messageChecklist *>(message_->content.get());
      object("checklist", JsonChecklist(content->list_.get(), client_));
      break;
    }
    case td_api::messageChecklistTasksDone::ID: {
      auto content = static_cast<const td_api::messageChecklistTasksDone *>(message_->content.get());
      object("checklist_tasks_done", JsonChecklistTasksDone(content, message_->chat_id, client_));
      break;
    }
    case td_api::messageChecklistTasksAdded::ID: {
      auto content = static_cast<const td_api::messageChecklistTasksAdded *>(message_->content.get());
      object("checklist_tasks_added", JsonChecklistTasksAdded(content, message_->chat_id, client_));
      break;
    }
    case td_api::messageGiftedGrams::ID:
      break;
    case td_api::messageSuggestedPostApprovalFailed::ID: {
      auto content = static_cast<const td_api::messageSuggestedPostApprovalFailed *>(message_->content.get());
      object("suggested_post_approval_failed", JsonSuggestedPostApprovalFailed(content, message_->chat_id, client_));
      break;
    }
    case td_api::messageSuggestedPostApproved::ID: {
      auto content = static_cast<const td_api::messageSuggestedPostApproved *>(message_->content.get());
      object("suggested_post_approved", JsonSuggestedPostApproved(content, message_->chat_id, client_));
      break;
    }
    case td_api::messageSuggestedPostDeclined::ID: {
      auto content = static_cast<const td_api::messageSuggestedPostDeclined *>(message_->content.get());
      object("suggested_post_declined", JsonSuggestedPostDeclined(content, message_->chat_id, client_));
      break;
    }
    case td_api::messageSuggestedPostPaid::ID: {
      auto content = static_cast<const td_api::messageSuggestedPostPaid *>(message_->content.get());
      object("suggested_post_paid", JsonSuggestedPostPaid(content, message_->chat_id, client_));
      break;
    }
    case td_api::messageSuggestedPostRefunded::ID: {
      auto content = static_cast<const td_api::messageSuggestedPostRefunded *>(message_->content.get());
      object("suggested_post_refunded", JsonSuggestedPostRefunded(content, message_->chat_id, client_));
      break;
    }
    case td_api::messageSuggestBirthdate::ID:
      break;
    case td_api::messageUpgradedGiftPurchaseOffer::ID:
      break;
    case td_api::messageUpgradedGiftPurchaseOfferRejected::ID:
      break;
    case td_api::messageChatOwnerLeft::ID: {
      auto content = static_cast<const td_api::messageChatOwnerLeft *>(message_->content.get());
      object("chat_owner_left", JsonChatOwnerLeft(content, client_));
      break;
    }
    case td_api::messageChatOwnerChanged::ID: {
      auto content = static_cast<const td_api::messageChatOwnerChanged *>(message_->content.get());
      object("chat_owner_changed", JsonChatOwnerChanged(content, client_));
      break;
    }
    case td_api::messageChatHasProtectedContentToggled::ID:
      break;
    case td_api::messageChatHasProtectedContentDisableRequested::ID:
      break;
    case td_api::messagePollOptionAdded::ID: {
      auto content = static_cast<const td_api::messagePollOptionAdded *>(message_->content.get());
      object("poll_option_added", JsonPollOptionAdded(content, message_->chat_id, need_reply_, client_));
      break;
    }
    case td_api::messagePollOptionDeleted::ID: {
      auto content = static_cast<const td_api::messagePollOptionDeleted *>(message_->content.get());
      object("poll_option_deleted", JsonPollOptionDeleted(content, message_->chat_id, need_reply_, client_));
      break;
    }
    case td_api::messageManagedBotCreated::ID: {
      auto content = static_cast<const td_api::messageManagedBotCreated *>(message_->content.get());
      object("managed_bot_created", JsonManagedBotCreated(content, client_));
      break;
    }
    case td_api::messageRichMessage::ID: {
      auto content = static_cast<const td_api::messageRichMessage *>(message_->content.get());
      object("rich_message", JsonRichMessage(content->message_.get(), client_));
      break;
    }
    case td_api::messageChatAddedToCommunity::ID: {
      auto content = static_cast<const td_api::messageChatAddedToCommunity *>(message_->content.get());
      object("community_chat_added", JsonCommunityChatAdded(content, client_));
      break;
    }
    case td_api::messageChatRemovedFromCommunity::ID:
      object("community_chat_removed", JsonEmptyObject());
      break;
    case td_api::messageChatJoinFromCommunity::ID: {
      auto content = static_cast<const td_api::messageChatJoinFromCommunity *>(message_->content.get());
      object("community_chat_joined", JsonCommunityChatJoined(content, client_));
      break;
    }
    default:
      UNREACHABLE();
  }
  if (message_->reply_markup != nullptr) {
    object("reply_markup", JsonReplyMarkup(message_->reply_markup.get()));
  }
  if (message_->via_bot_user_id > 0) {
    object("via_bot", JsonUser(message_->via_bot_user_id, client_));
  }
  if (message_->guest_bot_caller_id != nullptr) {
    client_->json_store_message_sender(object, message_->guest_bot_caller_id, "guest_bot_caller_user",
                                       "guest_bot_caller_chat");
  }
  if (!message_->can_be_saved) {
    object("has_protected_content", td::JsonTrue());
  }
  if (is_topic_message(message_->topic_id)) {
    object("is_topic_message", td::JsonTrue());
  }
  if (message_->topic_id != nullptr && message_->topic_id->get_id() == td_api::messageTopicDirectMessages::ID) {
    auto topic_id = static_cast<const td_api::messageTopicDirectMessages *>(message_->topic_id.get())
                        ->direct_messages_chat_topic_id_;
    const auto *user_info = client_->get_user_info(topic_id);
    if (user_info == nullptr) {
      LOG(ERROR) << "Can't find user " << topic_id << " for direct messages topic";
    } else {
      object("direct_messages_topic", JsonDirectMessagesTopic(topic_id, client_));
    }
  }
  if (message_->is_from_offline) {
    object("is_from_offline", td::JsonTrue());
  }
  if (message_->effect_id != 0) {
    object("effect_id", td::to_string(message_->effect_id));
  }
  if (message_->sender_boost_count != 0) {
    object("sender_boost_count", message_->sender_boost_count);
  }
  if (!message_->sender_tag.empty()) {
    object("sender_tag", message_->sender_tag);
  }
  if (message_->paid_message_star_count != 0) {
    object("paid_star_count", message_->paid_message_star_count);
  }
  if (message_->is_paid_post) {
    object("is_paid_post", td::JsonTrue());
  }
  if (message_->suggested_post_info != nullptr) {
    object("suggested_post_info", JsonSuggestedPostInfo(message_->suggested_post_info.get()));
  }
}

class Client::JsonMessageId final : public td::Jsonable {
 public:
  explicit JsonMessageId(int64 message_id) : message_id_(message_id) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("message_id", as_client_message_id_unchecked(message_id_));
  }

 private:
  int64 message_id_;
};

class Client::JsonInlineQuery final : public td::Jsonable {
 public:
  JsonInlineQuery(int64 inline_query_id, int64 sender_user_id, const td_api::location *user_location,
                  const td_api::ChatType *chat_type, const td::string &query, const td::string &offset,
                  const Client *client)
      : inline_query_id_(inline_query_id)
      , sender_user_id_(sender_user_id)
      , user_location_(user_location)
      , chat_type_(chat_type)
      , query_(query)
      , offset_(offset)
      , client_(client) {
  }

  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(inline_query_id_));
    object("from", JsonUser(sender_user_id_, client_));
    if (user_location_ != nullptr) {
      object("location", JsonLocation(user_location_));
    }
    if (chat_type_ != nullptr) {
      auto chat_type = [&] {
        switch (chat_type_->get_id()) {
          case td_api::chatTypePrivate::ID: {
            auto type = static_cast<const td_api::chatTypePrivate *>(chat_type_);
            if (type->user_id_ == sender_user_id_) {
              return "sender";
            }
            return "private";
          }
          case td_api::chatTypeBasicGroup::ID:
            return "group";
          case td_api::chatTypeSupergroup::ID: {
            auto type = static_cast<const td_api::chatTypeSupergroup *>(chat_type_);
            if (type->is_channel_) {
              return "channel";
            } else {
              return "supergroup";
            }
          }
          case td_api::chatTypeSecret::ID:
            return "";
          default:
            UNREACHABLE();
            return "";
        }
      }();
      if (chat_type[0] != '\0') {
        object("chat_type", chat_type);
      }
    }
    object("query", query_);
    object("offset", offset_);
  }

 private:
  int64 inline_query_id_;
  int64 sender_user_id_;
  const td_api::location *user_location_;
  const td_api::ChatType *chat_type_;
  const td::string &query_;
  const td::string &offset_;
  const Client *client_;
};

class Client::JsonChosenInlineResult final : public td::Jsonable {
 public:
  JsonChosenInlineResult(int64 sender_user_id, const td_api::location *user_location, const td::string &query,
                         const td::string &result_id, const td::string &inline_message_id, const Client *client)
      : sender_user_id_(sender_user_id)
      , user_location_(user_location)
      , query_(query)
      , result_id_(result_id)
      , inline_message_id_(inline_message_id)
      , client_(client) {
  }

  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("from", JsonUser(sender_user_id_, client_));
    if (user_location_ != nullptr) {
      object("location", JsonLocation(user_location_));
    }
    if (!inline_message_id_.empty()) {
      object("inline_message_id", inline_message_id_);
    }
    object("query", query_);
    object("result_id", result_id_);
  }

 private:
  int64 sender_user_id_;
  const td_api::location *user_location_;
  const td::string &query_;
  const td::string &result_id_;
  const td::string &inline_message_id_;
  const Client *client_;
};

class Client::JsonCallbackQuery final : public td::Jsonable {
 public:
  JsonCallbackQuery(int64 callback_query_id, int64 sender_user_id, int64 chat_id, int64 message_id,
                    const MessageInfo *message_info, int64 chat_instance, td_api::CallbackQueryPayload *payload,
                    const Client *client)
      : callback_query_id_(callback_query_id)
      , sender_user_id_(sender_user_id)
      , chat_id_(chat_id)
      , message_id_(message_id)
      , message_info_(message_info)
      , chat_instance_(chat_instance)
      , payload_(payload)
      , client_(client) {
  }

  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(callback_query_id_));
    object("from", JsonUser(sender_user_id_, client_));
    if (message_info_ != nullptr) {
      object("message", JsonMessage(message_info_, true, "callback query", client_));
    } else {
      object("message", JsonInaccessibleMessage(chat_id_, message_id_, client_));
    }
    object("chat_instance", td::to_string(chat_instance_));
    client_->json_store_callback_query_payload(object, payload_);
  }

 private:
  int64 callback_query_id_;
  int64 sender_user_id_;
  int64 chat_id_;
  int64 message_id_;
  const MessageInfo *message_info_;
  int64 chat_instance_;
  td_api::CallbackQueryPayload *payload_;
  const Client *client_;
};

class Client::JsonInlineCallbackQuery final : public td::Jsonable {
 public:
  JsonInlineCallbackQuery(int64 callback_query_id, int64 sender_user_id, const td::string &inline_message_id,
                          int64 chat_instance, td_api::CallbackQueryPayload *payload, const Client *client)
      : callback_query_id_(callback_query_id)
      , sender_user_id_(sender_user_id)
      , inline_message_id_(inline_message_id)
      , chat_instance_(chat_instance)
      , payload_(payload)
      , client_(client) {
  }

  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(callback_query_id_));
    object("from", JsonUser(sender_user_id_, client_));
    object("inline_message_id", inline_message_id_);
    object("chat_instance", td::to_string(chat_instance_));
    client_->json_store_callback_query_payload(object, payload_);
  }

 private:
  int64 callback_query_id_;
  int64 sender_user_id_;
  const td::string &inline_message_id_;
  int64 chat_instance_;
  td_api::CallbackQueryPayload *payload_;
  const Client *client_;
};

class Client::JsonShippingQuery final : public td::Jsonable {
 public:
  JsonShippingQuery(const td_api::updateNewShippingQuery *query, const Client *client)
      : query_(query), client_(client) {
  }

  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(query_->id_));
    object("from", JsonUser(query_->sender_user_id_, client_));
    if (!td::check_utf8(query_->invoice_payload_)) {
      LOG(WARNING) << "Receive non-UTF-8 invoice payload";
      object("invoice_payload", td::JsonRawString(query_->invoice_payload_));
    } else {
      object("invoice_payload", query_->invoice_payload_);
    }
    object("shipping_address", JsonAddress(query_->shipping_address_.get()));
  }

 private:
  const td_api::updateNewShippingQuery *query_;
  const Client *client_;
};

class Client::JsonPreCheckoutQuery final : public td::Jsonable {
 public:
  JsonPreCheckoutQuery(const td_api::updateNewPreCheckoutQuery *query, const Client *client)
      : query_(query), client_(client) {
  }

  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::to_string(query_->id_));
    object("from", JsonUser(query_->sender_user_id_, client_));
    object("currency", query_->currency_);
    object("total_amount", query_->total_amount_);
    if (!td::check_utf8(query_->invoice_payload_)) {
      LOG(WARNING) << "Receive non-UTF-8 invoice payload";
      object("invoice_payload", td::JsonRawString(query_->invoice_payload_));
    } else {
      object("invoice_payload", query_->invoice_payload_);
    }
    if (!query_->shipping_option_id_.empty()) {
      object("shipping_option_id", query_->shipping_option_id_);
    }
    if (query_->order_info_ != nullptr) {
      object("order_info", JsonOrderInfo(query_->order_info_.get()));
    }
  }

 private:
  const td_api::updateNewPreCheckoutQuery *query_;
  const Client *client_;
};

class Client::JsonPaidMediaPurchased final : public td::Jsonable {
 public:
  JsonPaidMediaPurchased(const td_api::updatePaidMediaPurchased *update, const Client *client)
      : update_(update), client_(client) {
  }

  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("from", JsonUser(update_->user_id_, client_));
    object("paid_media_payload", update_->payload_);
  }

 private:
  const td_api::updatePaidMediaPurchased *update_;
  const Client *client_;
};

class Client::JsonCustomJson final : public td::Jsonable {
 public:
  explicit JsonCustomJson(const td::string &json) : json_(json) {
  }

  void store(td::JsonValueScope *scope) const {
    *scope << td::JsonRaw(json_);
  }

 private:
  const td::string &json_;
};

class Client::JsonBotCommand final : public td::Jsonable {
 public:
  explicit JsonBotCommand(const td_api::botCommand *command) : command_(command) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("command", command_->command_);
    object("description", command_->description_);
    if (command_->is_ephemeral_) {
      object("is_ephemeral", td::JsonTrue());
    }
  }

 private:
  const td_api::botCommand *command_;
};

class Client::JsonBotMenuButton final : public td::Jsonable {
 public:
  explicit JsonBotMenuButton(const td_api::botMenuButton *menu_button) : menu_button_(menu_button) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (menu_button_->text_.empty()) {
      object("type", menu_button_->url_.empty() ? "commands" : "default");
    } else {
      object("type", "web_app");
      object("text", menu_button_->text_);
      object("web_app", JsonWebAppInfo(menu_button_->url_));
    }
  }

 private:
  const td_api::botMenuButton *menu_button_;
};

class Client::JsonBotName final : public td::Jsonable {
 public:
  explicit JsonBotName(const td_api::text *text) : text_(text) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("name", text_->text_);
  }

 private:
  const td_api::text *text_;
};

class Client::JsonBotInfoDescription final : public td::Jsonable {
 public:
  explicit JsonBotInfoDescription(const td_api::text *text) : text_(text) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("description", text_->text_);
  }

 private:
  const td_api::text *text_;
};

class Client::JsonBotInfoShortDescription final : public td::Jsonable {
 public:
  explicit JsonBotInfoShortDescription(const td_api::text *text) : text_(text) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("short_description", text_->text_);
  }

 private:
  const td_api::text *text_;
};

class Client::JsonChatAdministratorRights final : public td::Jsonable {
 public:
  JsonChatAdministratorRights(const td_api::chatAdministratorRights *rights, ChatType chat_type)
      : rights_(rights), chat_type_(chat_type) {
  }

  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    td_api::chatAdministratorRights empty_rights;
    json_store_administrator_rights(object, rights_ == nullptr ? &empty_rights : rights_, chat_type_);
  }

 private:
  const td_api::chatAdministratorRights *rights_;
  ChatType chat_type_;
};

class Client::JsonChatPhotos final : public td::Jsonable {
 public:
  JsonChatPhotos(const td_api::chatPhotos *photos, const Client *client) : photos_(photos), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("total_count", photos_->total_count_);
    object("photos", td::json_array(photos_->photos_,
                                    [client = client_](auto &photo) { return JsonChatPhoto(photo.get(), client); }));
  }

 private:
  const td_api::chatPhotos *photos_;
  const Client *client_;
};

class Client::JsonChatMember final : public td::Jsonable {
 public:
  JsonChatMember(const td_api::chatMember *member, ChatType chat_type, const Client *client)
      : member_(member), chat_type_(chat_type), client_(client) {
  }

  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    CHECK(member_->member_id_ != nullptr);
    switch (member_->member_id_->get_id()) {
      case td_api::messageSenderUser::ID: {
        auto user_id = static_cast<const td_api::messageSenderUser *>(member_->member_id_.get())->user_id_;
        object("user", JsonUser(user_id, client_));
        break;
      }
      case td_api::messageSenderChat::ID:
        break;
      default:
        UNREACHABLE();
    }
    object("status", get_chat_member_status(member_->status_));
    switch (member_->status_->get_id()) {
      case td_api::chatMemberStatusCreator::ID: {
        auto creator = static_cast<const td_api::chatMemberStatusCreator *>(member_->status_.get());
        if (!member_->tag_.empty()) {
          object("custom_title", member_->tag_);
        }
        object("is_anonymous", td::JsonBool(creator->is_anonymous_));
        // object("is_member", creator->is_member_); only creator itself knows that he is a left creator
        break;
      }
      case td_api::chatMemberStatusAdministrator::ID: {
        auto administrator = static_cast<const td_api::chatMemberStatusAdministrator *>(member_->status_.get());
        object("can_be_edited", td::JsonBool(administrator->can_be_edited_));
        json_store_administrator_rights(object, administrator->rights_.get(), chat_type_);
        object("can_manage_voice_chats", td::JsonBool(administrator->rights_->can_manage_video_chats_));
        if (!member_->tag_.empty()) {
          object("custom_title", member_->tag_);
        }
        break;
      }
      case td_api::chatMemberStatusMember::ID: {
        auto member = static_cast<const td_api::chatMemberStatusMember *>(member_->status_.get());
        if (!member_->tag_.empty()) {
          object("tag", member_->tag_);
        }
        if (member->member_until_date_ > 0) {
          object("until_date", member->member_until_date_);
        }
        break;
      }
      case td_api::chatMemberStatusRestricted::ID:
        if (!member_->tag_.empty()) {
          object("tag", member_->tag_);
        }
        if (chat_type_ == ChatType::Supergroup) {
          auto restricted = static_cast<const td_api::chatMemberStatusRestricted *>(member_->status_.get());
          object("until_date", restricted->restricted_until_date_);
          json_store_permissions(object, restricted->permissions_.get());
          object("is_member", td::JsonBool(restricted->is_member_));
        }
        break;
      case td_api::chatMemberStatusLeft::ID:
        break;
      case td_api::chatMemberStatusBanned::ID: {
        auto banned = static_cast<const td_api::chatMemberStatusBanned *>(member_->status_.get());
        object("until_date", banned->banned_until_date_);
        break;
      }
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::chatMember *member_;
  ChatType chat_type_;
  const Client *client_;
};

class Client::JsonChatMembers final : public td::Jsonable {
 public:
  JsonChatMembers(const td::vector<object_ptr<td_api::chatMember>> &members, ChatType chat_type,
                  bool administrators_only, bool return_bots, const Client *client)
      : members_(members)
      , chat_type_(chat_type)
      , administrators_only_(administrators_only)
      , return_bots_(return_bots)
      , client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (auto &member : members_) {
      CHECK(member != nullptr);
      CHECK(member->member_id_ != nullptr);
      if (member->member_id_->get_id() != td_api::messageSenderUser::ID) {
        continue;
      }
      if (!return_bots_) {
        auto user_id = static_cast<const td_api::messageSenderUser *>(member->member_id_.get())->user_id_;
        auto user_info = client_->get_user_info(user_id);
        bool is_member_bot = user_info != nullptr && user_info->type == UserInfo::Type::Bot;
        if (is_member_bot && user_id != client_->my_id_) {
          continue;
        }
      }
      if (administrators_only_) {
        auto status = get_chat_member_status(member->status_);
        if (status != "creator" && status != "administrator") {
          continue;
        }
      }
      array << JsonChatMember(member.get(), chat_type_, client_);
    }
  }

 private:
  const td::vector<object_ptr<td_api::chatMember>> &members_;
  ChatType chat_type_;
  bool administrators_only_;
  bool return_bots_;
  const Client *client_;
};

class Client::JsonChatMemberUpdated final : public td::Jsonable {
 public:
  JsonChatMemberUpdated(const td_api::updateChatMember *update, const Client *client)
      : update_(update), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("chat", JsonChat(update_->chat_id_, client_));
    object("from", JsonUser(update_->actor_user_id_, client_));
    object("date", update_->date_);
    auto chat_type = client_->get_chat_type(update_->chat_id_);
    object("old_chat_member", JsonChatMember(update_->old_chat_member_.get(), chat_type, client_));
    object("new_chat_member", JsonChatMember(update_->new_chat_member_.get(), chat_type, client_));
    if (update_->invite_link_ != nullptr) {
      object("invite_link", JsonChatInviteLink(update_->invite_link_.get(), client_));
    }
    if (update_->via_join_request_) {
      object("via_join_request", td::JsonTrue());
    }
    if (update_->via_chat_folder_invite_link_) {
      object("via_chat_folder_invite_link", td::JsonTrue());
    }
  }

 private:
  const td_api::updateChatMember *update_;
  const Client *client_;
};

class Client::JsonChatJoinRequest final : public td::Jsonable {
 public:
  JsonChatJoinRequest(const td_api::updateNewChatJoinRequest *update, const Client *client)
      : update_(update), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("chat", JsonChat(update_->chat_id_, client_));
    object("from", JsonUser(update_->request_->user_id_, client_));
    object("user_chat_id", update_->user_chat_id_);
    object("date", update_->request_->date_);
    if (!update_->request_->bio_.empty()) {
      object("bio", update_->request_->bio_);
    }
    if (update_->invite_link_ != nullptr) {
      object("invite_link", JsonChatInviteLink(update_->invite_link_.get(), client_));
    }
    if (update_->query_id_ != 0) {
      object("query_id", td::to_string(update_->query_id_));
    }
  }

 private:
  const td_api::updateNewChatJoinRequest *update_;
  const Client *client_;
};

class Client::JsonChatBoostSource final : public td::Jsonable {
 public:
  JsonChatBoostSource(const td_api::ChatBoostSource *boost_source, const Client *client)
      : boost_source_(boost_source), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    CHECK(boost_source_ != nullptr);
    switch (boost_source_->get_id()) {
      case td_api::chatBoostSourcePremium::ID: {
        const auto *source = static_cast<const td_api::chatBoostSourcePremium *>(boost_source_);
        object("source", "premium");
        object("user", JsonUser(source->user_id_, client_));
        break;
      }
      case td_api::chatBoostSourceGiftCode::ID: {
        const auto *source = static_cast<const td_api::chatBoostSourceGiftCode *>(boost_source_);
        object("source", "gift_code");
        object("user", JsonUser(source->user_id_, client_));
        break;
      }
      case td_api::chatBoostSourceGiveaway::ID: {
        const auto *source = static_cast<const td_api::chatBoostSourceGiveaway *>(boost_source_);
        object("source", "giveaway");
        object("giveaway_message_id", as_client_message_id_unchecked(source->giveaway_message_id_));
        if (source->star_count_ > 0) {
          object("prize_star_count", source->star_count_);
        }
        if (source->user_id_ != 0) {
          object("user", JsonUser(source->user_id_, client_));
        } else if (source->is_unclaimed_) {
          object("is_unclaimed", td::JsonTrue());
        }
        break;
      }
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::ChatBoostSource *boost_source_;
  const Client *client_;
};

class Client::JsonChatBoost final : public td::Jsonable {
 public:
  JsonChatBoost(const td_api::chatBoost *boost, const Client *client) : boost_(boost), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("boost_id", boost_->id_);
    object("add_date", boost_->start_date_);
    object("expiration_date", boost_->expiration_date_);
    object("source", JsonChatBoostSource(boost_->source_.get(), client_));
  }

 private:
  const td_api::chatBoost *boost_;
  const Client *client_;
};

class Client::JsonChatBoostUpdated final : public td::Jsonable {
 public:
  JsonChatBoostUpdated(const td_api::updateChatBoost *update, const Client *client) : update_(update), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("chat", JsonChat(update_->chat_id_, client_));
    object("boost", JsonChatBoost(update_->boost_.get(), client_));
  }

 private:
  const td_api::updateChatBoost *update_;
  const Client *client_;
};

class Client::JsonChatBoostRemoved final : public td::Jsonable {
 public:
  JsonChatBoostRemoved(const td_api::updateChatBoost *update, const Client *client) : update_(update), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("chat", JsonChat(update_->chat_id_, client_));
    object("boost_id", update_->boost_->id_);
    object("remove_date", update_->boost_->start_date_);
    object("source", JsonChatBoostSource(update_->boost_->source_.get(), client_));
  }

 private:
  const td_api::updateChatBoost *update_;
  const Client *client_;
};

class Client::JsonChatBoosts final : public td::Jsonable {
 public:
  JsonChatBoosts(const td_api::foundChatBoosts *chat_boosts, const Client *client)
      : chat_boosts_(chat_boosts), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("boosts", td::json_array(chat_boosts_->boosts_,
                                    [client = client_](auto &boost) { return JsonChatBoost(boost.get(), client); }));
  }

 private:
  const td_api::foundChatBoosts *chat_boosts_;
  const Client *client_;
};

class Client::JsonGameHighScore final : public td::Jsonable {
 public:
  JsonGameHighScore(const td_api::gameHighScore *score, const Client *client) : score_(score), client_(client) {
  }

  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("position", score_->position_);
    object("user", JsonUser(score_->user_id_, client_));
    object("score", score_->score_);
  }

 private:
  const td_api::gameHighScore *score_;
  const Client *client_;
};

class Client::JsonMessageReactionUpdated final : public td::Jsonable {
 public:
  JsonMessageReactionUpdated(const td_api::updateMessageReaction *update, const Client *client)
      : update_(update), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("chat", JsonChat(update_->chat_id_, client_));
    object("message_id", as_client_message_id(update_->message_id_));
    client_->json_store_message_sender(object, update_->actor_id_, "user", "actor_chat");
    object("date", update_->date_);
    object("old_reaction", td::json_array(update_->old_reaction_types_,
                                          [](const auto &reaction) { return JsonReactionType(reaction.get()); }));
    object("new_reaction", td::json_array(update_->new_reaction_types_,
                                          [](const auto &reaction) { return JsonReactionType(reaction.get()); }));
  }

 private:
  const td_api::updateMessageReaction *update_;
  const Client *client_;
};

class Client::JsonMessageReactionCountUpdated final : public td::Jsonable {
 public:
  JsonMessageReactionCountUpdated(const td_api::updateMessageReactions *update, const Client *client)
      : update_(update), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("chat", JsonChat(update_->chat_id_, client_));
    object("message_id", as_client_message_id(update_->message_id_));
    object("date", update_->date_);
    object("reactions",
           td::json_array(update_->reactions_, [](const auto &reaction) { return JsonReactionCount(reaction.get()); }));
  }

 private:
  const td_api::updateMessageReactions *update_;
  const Client *client_;
};

class Client::JsonBusinessBotRights final : public td::Jsonable {
 public:
  explicit JsonBusinessBotRights(const td_api::businessBotRights *rights) : rights_(rights) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (rights_->can_reply_) {
      object("can_reply", td::JsonTrue());
    }
    if (rights_->can_read_messages_) {
      object("can_read_messages", td::JsonTrue());
    }
    if (rights_->can_delete_sent_messages_) {
      object("can_delete_sent_messages", td::JsonTrue());
    }
    if (rights_->can_delete_all_messages_) {
      object("can_delete_all_messages", td::JsonTrue());
    }
    if (rights_->can_edit_name_) {
      object("can_edit_name", td::JsonTrue());
    }
    if (rights_->can_edit_bio_) {
      object("can_edit_bio", td::JsonTrue());
    }
    if (rights_->can_edit_profile_photo_) {
      object("can_edit_profile_photo", td::JsonTrue());
    }
    if (rights_->can_edit_username_) {
      object("can_edit_username", td::JsonTrue());
    }
    if (rights_->can_view_gifts_and_stars_) {
      object("can_view_gifts_and_stars", td::JsonTrue());
    }
    if (rights_->can_sell_gifts_) {
      object("can_convert_gifts_to_stars", td::JsonTrue());
    }
    if (rights_->can_change_gift_settings_) {
      object("can_change_gift_settings", td::JsonTrue());
    }
    if (rights_->can_transfer_and_upgrade_gifts_) {
      object("can_transfer_and_upgrade_gifts", td::JsonTrue());
    }
    if (rights_->can_transfer_stars_) {
      object("can_transfer_stars", td::JsonTrue());
    }
    if (rights_->can_manage_stories_) {
      object("can_manage_stories", td::JsonTrue());
    }
  }

 private:
  const td_api::businessBotRights *rights_;
};

class Client::JsonBusinessConnection final : public td::Jsonable {
 public:
  JsonBusinessConnection(const BusinessConnection *connection, const Client *client)
      : connection_(connection), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", connection_->id_);
    object("user", JsonUser(connection_->user_id_, client_));
    object("user_chat_id", connection_->user_chat_id_);
    object("date", connection_->date_);
    object("is_enabled", td::JsonBool(connection_->is_enabled_));
    if (connection_->rights_ != nullptr) {
      object("can_reply", td::JsonBool(connection_->rights_->can_reply_));
      object("rights", JsonBusinessBotRights(connection_->rights_.get()));
    } else {
      object("can_reply", td::JsonBool(false));
    }
  }

 private:
  const BusinessConnection *connection_;
  const Client *client_;
};

class Client::JsonBusinessMessagesDeleted final : public td::Jsonable {
 public:
  JsonBusinessMessagesDeleted(const td_api::updateBusinessMessagesDeleted *update, const Client *client)
      : update_(update), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("business_connection_id", update_->connection_id_);
    object("chat", JsonChat(update_->chat_id_, client_));
    object("message_ids", td::json_array(update_->message_ids_, as_client_message_id));
  }

 private:
  const td_api::updateBusinessMessagesDeleted *update_;
  const Client *client_;
};

class Client::JsonReceivedGift final : public td::Jsonable {
 public:
  JsonReceivedGift(const td_api::receivedGift *received_gift, bool can_be_managed, const Client *client)
      : received_gift_(received_gift), can_be_managed_(can_be_managed), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (!received_gift_->received_gift_id_.empty() && can_be_managed_) {
      object("owned_gift_id", received_gift_->received_gift_id_);
    }
    switch (received_gift_->gift_->get_id()) {
      case td_api::sentGiftRegular::ID: {
        auto gift = static_cast<const td_api::sentGiftRegular *>(received_gift_->gift_.get());
        object("type", "regular");
        object("gift", JsonGift(gift->gift_.get(), client_));
        break;
      }
      case td_api::sentGiftUpgraded::ID: {
        auto gift = static_cast<const td_api::sentGiftUpgraded *>(received_gift_->gift_.get());
        object("type", "unique");
        object("gift", JsonUniqueGift(gift->gift_.get(), client_));
        break;
      }
      default:
        UNREACHABLE();
    }
    if (received_gift_->sender_id_ != nullptr) {
      client_->json_store_message_sender(object, received_gift_->sender_id_, "sender_user", "sender_chat");
    }
    object("send_date", received_gift_->date_);
    if (!received_gift_->text_->text_.empty()) {
      object("text", received_gift_->text_->text_);
    }
    if (!received_gift_->text_->entities_.empty()) {
      object("entities", JsonVectorEntities(received_gift_->text_->entities_, client_));
    }
    if (received_gift_->is_private_) {
      object("is_private", td::JsonTrue());
    }
    if (received_gift_->is_saved_) {
      object("is_saved", td::JsonTrue());
    }
    if (received_gift_->can_be_upgraded_) {
      object("can_be_upgraded", td::JsonTrue());
    }
    if (received_gift_->can_be_transferred_) {
      object("can_be_transferred", td::JsonTrue());
    }
    if (received_gift_->was_refunded_) {
      object("was_refunded", td::JsonTrue());
    }
    if (received_gift_->sell_star_count_ > 0) {
      object("convert_star_count", received_gift_->sell_star_count_);
    }
    if (received_gift_->prepaid_upgrade_star_count_ > 0) {
      object("prepaid_upgrade_star_count", received_gift_->prepaid_upgrade_star_count_);
    }
    if (received_gift_->transfer_star_count_ > 0) {
      object("transfer_star_count", received_gift_->transfer_star_count_);
    }
    if (received_gift_->next_transfer_date_ > 0) {
      object("next_transfer_date", received_gift_->next_transfer_date_);
    }
    if (received_gift_->is_upgrade_separate_) {
      object("is_upgrade_separate", td::JsonTrue());
    }
    if (received_gift_->unique_gift_number_ > 0) {
      object("unique_gift_number", received_gift_->unique_gift_number_);
    }
  }

 private:
  const td_api::receivedGift *received_gift_;
  bool can_be_managed_;
  const Client *client_;
};

class Client::JsonReceivedGifts final : public td::Jsonable {
 public:
  JsonReceivedGifts(const td_api::receivedGifts *received_gifts, bool can_be_managed, const Client *client)
      : received_gifts_(received_gifts), can_be_managed_(can_be_managed), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("total_count", received_gifts_->total_count_);
    object("gifts", td::json_array(received_gifts_->gifts_,
                                   [can_be_managed = can_be_managed_, client = client_](const auto &received_gift) {
                                     return JsonReceivedGift(received_gift.get(), can_be_managed, client);
                                   }));
    if (!received_gifts_->next_offset_.empty()) {
      object("next_offset", received_gifts_->next_offset_);
    }
  }

 private:
  const td_api::receivedGifts *received_gifts_;
  bool can_be_managed_;
  const Client *client_;
};

class Client::JsonRevenueWithdrawalState final : public td::Jsonable {
 public:
  explicit JsonRevenueWithdrawalState(const td_api::RevenueWithdrawalState *state) : state_(state) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    switch (state_->get_id()) {
      case td_api::revenueWithdrawalStatePending::ID:
        object("type", "pending");
        break;
      case td_api::revenueWithdrawalStateSucceeded::ID: {
        auto state = static_cast<const td_api::revenueWithdrawalStateSucceeded *>(state_);
        object("type", "succeeded");
        object("date", state->date_);
        object("url", state->url_);
        break;
      }
      case td_api::revenueWithdrawalStateFailed::ID:
        object("type", "failed");
        break;
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::RevenueWithdrawalState *state_;
};

class Client::JsonAffiliateInfo final : public td::Jsonable {
 public:
  JsonAffiliateInfo(const td_api::affiliateInfo *affiliate_info, const Client *client)
      : affiliate_info_(affiliate_info), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    auto chat_info = client_->get_chat(affiliate_info_->affiliate_chat_id_);
    CHECK(chat_info != nullptr);
    if (chat_info->type == ChatInfo::Type::Private) {
      object("affiliate_user", JsonUser(chat_info->user_id, client_));
    } else {
      object("affiliate_chat", JsonChat(affiliate_info_->affiliate_chat_id_, client_));
    }
    object("commission_per_mille", affiliate_info_->commission_per_mille_);
    object("amount", affiliate_info_->star_amount_->star_count_);
    if (affiliate_info_->star_amount_->nanostar_count_ != 0) {
      object("nanostar_amount", affiliate_info_->star_amount_->nanostar_count_);
    }
  }

 private:
  const td_api::affiliateInfo *affiliate_info_;
  const Client *client_;
};

class Client::JsonStarTransactionType final : public td::Jsonable {
 public:
  JsonStarTransactionType(const td_api::StarTransactionType *type, const Client *client)
      : type_(type), client_(client) {
  }

  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    switch (type_->get_id()) {
      case td_api::starTransactionTypeFragmentDeposit::ID:
        object("type", "fragment");
        break;
      case td_api::starTransactionTypeFragmentWithdrawal::ID: {
        auto type = static_cast<const td_api::starTransactionTypeFragmentWithdrawal *>(type_);
        object("type", "fragment");
        if (type->withdrawal_state_ != nullptr) {
          object("withdrawal_state", JsonRevenueWithdrawalState(type->withdrawal_state_.get()));
        }
        break;
      }
      case td_api::starTransactionTypeBotPaidMediaSale::ID: {
        auto type = static_cast<const td_api::starTransactionTypeBotPaidMediaSale *>(type_);
        object("transaction_type", "paid_media_payment");
        object("type", "user");
        object("user", JsonUser(type->user_id_, client_));
        object("paid_media", td::json_array(type->media_, [client = client_](auto &media) {
                 return JsonPaidMedia(media.get(), client);
               }));
        if (!type->payload_.empty()) {
          object("paid_media_payload", type->payload_);
        }
        if (type->affiliate_ != nullptr) {
          object("affiliate", JsonAffiliateInfo(type->affiliate_.get(), client_));
        }
        break;
      }
      case td_api::starTransactionTypeBotInvoiceSale::ID: {
        auto type = static_cast<const td_api::starTransactionTypeBotInvoiceSale *>(type_);
        object("transaction_type", "invoice_payment");
        object("type", "user");
        object("user", JsonUser(type->user_id_, client_));
        if (!type->invoice_payload_.empty()) {
          if (!td::check_utf8(type->invoice_payload_)) {
            LOG(WARNING) << "Receive non-UTF-8 invoice payload";
            object("invoice_payload", td::JsonRawString(type->invoice_payload_));
          } else {
            object("invoice_payload", type->invoice_payload_);
          }
        }
        if (type->affiliate_ != nullptr) {
          object("affiliate", JsonAffiliateInfo(type->affiliate_.get(), client_));
        }
        break;
      }
      case td_api::starTransactionTypeBotSubscriptionSale::ID: {
        auto type = static_cast<const td_api::starTransactionTypeBotSubscriptionSale *>(type_);
        object("transaction_type", "invoice_payment");
        object("type", "user");
        object("user", JsonUser(type->user_id_, client_));
        if (!type->invoice_payload_.empty()) {
          if (!td::check_utf8(type->invoice_payload_)) {
            LOG(WARNING) << "Receive non-UTF-8 invoice payload";
            object("invoice_payload", td::JsonRawString(type->invoice_payload_));
          } else {
            object("invoice_payload", type->invoice_payload_);
          }
        }
        if (type->subscription_period_ > 0) {
          object("subscription_period", type->subscription_period_);
        }
        if (type->affiliate_ != nullptr) {
          object("affiliate", JsonAffiliateInfo(type->affiliate_.get(), client_));
        }
        break;
      }
      case td_api::starTransactionTypeTelegramAdsWithdrawal::ID:
        object("type", "telegram_ads");
        break;
      case td_api::starTransactionTypeTelegramApiUsage::ID: {
        auto type = static_cast<const td_api::starTransactionTypeTelegramApiUsage *>(type_);
        object("type", "telegram_api");
        object("request_count", type->request_count_);
        break;
      }
      case td_api::starTransactionTypeGiftPurchase::ID: {
        auto type = static_cast<const td_api::starTransactionTypeGiftPurchase *>(type_);
        switch (type->owner_id_->get_id()) {
          case td_api::messageSenderUser::ID: {
            auto owner_id = static_cast<const td_api::messageSenderUser *>(type->owner_id_.get());
            object("transaction_type", "gift_purchase");
            object("type", "user");
            object("user", JsonUser(owner_id->user_id_, client_));
            break;
          }
          case td_api::messageSenderChat::ID: {
            auto owner_id = static_cast<const td_api::messageSenderChat *>(type->owner_id_.get());
            object("type", "chat");
            object("chat", JsonChat(owner_id->chat_id_, client_));
            break;
          }
          default:
            UNREACHABLE();
        }
        object("gift", JsonGift(type->gift_.get(), client_));
        break;
      }
      case td_api::starTransactionTypePremiumPurchase::ID: {
        auto type = static_cast<const td_api::starTransactionTypePremiumPurchase *>(type_);
        object("transaction_type", "premium_purchase");
        object("type", "user");
        object("user", JsonUser(type->user_id_, client_));
        object("premium_subscription_duration", type->month_count_);
        break;
      }
      case td_api::starTransactionTypeBusinessBotTransferReceive::ID: {
        auto type = static_cast<const td_api::starTransactionTypeBusinessBotTransferReceive *>(type_);
        object("transaction_type", "business_account_transfer");
        object("type", "user");
        object("user", JsonUser(type->user_id_, client_));
        break;
      }
      case td_api::starTransactionTypeAffiliateProgramCommission::ID: {
        auto type = static_cast<const td_api::starTransactionTypeAffiliateProgramCommission *>(type_);
        object("type", "affiliate_program");
        auto chat_info = client_->get_chat(type->chat_id_);
        CHECK(chat_info != nullptr);
        if (chat_info->type == ChatInfo::Type::Private) {
          object("sponsor_user", JsonUser(chat_info->user_id, client_));
        } else {
          LOG(ERROR) << "Receive chat " << type->chat_id_ << " as an affiliate program sponsor";
          object("sponsor_chat", JsonChat(type->chat_id_, client_));
        }
        object("commission_per_mille", type->commission_per_mille_);
        break;
      }
      case td_api::starTransactionTypeUnsupported::ID:
        object("type", "other");
        break;
      default:
        UNREACHABLE();
    }
  }

 private:
  const td_api::StarTransactionType *type_;
  const Client *client_;
};

class Client::JsonStarTransaction final : public td::Jsonable {
 public:
  JsonStarTransaction(const td_api::starTransaction *transaction, const Client *client)
      : transaction_(transaction), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", transaction_->id_);
    object("date", transaction_->date_);
    auto star_count = transaction_->star_amount_->star_count_;
    auto nanostar_count = transaction_->star_amount_->nanostar_count_;
    if (star_count > 0 || nanostar_count > 0) {
      object("source", JsonStarTransactionType(transaction_->type_.get(), client_));
    } else {
      star_count = -star_count;
      nanostar_count = -nanostar_count;
      object("receiver", JsonStarTransactionType(transaction_->type_.get(), client_));
    }
    object("amount", star_count);
    if (nanostar_count != 0) {
      object("nanostar_amount", nanostar_count);
    }
  }

 private:
  const td_api::starTransaction *transaction_;
  const Client *client_;
};

class Client::JsonStarTransactions final : public td::Jsonable {
 public:
  JsonStarTransactions(const td_api::starTransactions *transactions, const Client *client)
      : transactions_(transactions), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("transactions", td::json_array(transactions_->transactions_, [client = client_](const auto &transaction) {
             return JsonStarTransaction(transaction.get(), client);
           }));
  }

 private:
  const td_api::starTransactions *transactions_;
  const Client *client_;
};

class Client::JsonUpdateTypes final : public td::Jsonable {
 public:
  explicit JsonUpdateTypes(td::uint64 update_types) : update_types_(update_types) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (int32 i = 0; i < static_cast<int32>(UpdateType::Size); i++) {
      if (((update_types_ >> i) & 1) != 0) {
        auto update_type = static_cast<UpdateType>(i);
        if (update_type != UpdateType::CustomEvent && update_type != UpdateType::CustomQuery) {
          array << get_update_type_name(update_type);
        }
      }
    }
  }

 private:
  td::uint64 update_types_;
};

class Client::JsonWebhookInfo final : public td::Jsonable {
 public:
  explicit JsonWebhookInfo(const Client *client) : client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    td::CSlice url = client_->webhook_url_;
    if (td::check_utf8(url)) {
      object("url", url);
    } else {
      object("url", td::JsonRawString(url));
    }
    object("has_custom_certificate", td::JsonBool(client_->has_webhook_certificate_));
    object("pending_update_count", td::narrow_cast<int32>(client_->get_pending_update_count()));
    if (client_->last_webhook_error_date_ > 0) {
      object("last_error_date", client_->last_webhook_error_date_);
      td::CSlice error_message = client_->last_webhook_error_.message();
      if (td::check_utf8(error_message)) {
        object("last_error_message", error_message);
      } else {
        object("last_error_message", td::JsonRawString(error_message));
      }
    }
    if (client_->webhook_max_connections_ > 0) {
      object("max_connections", client_->webhook_max_connections_);
    }
    if (!url.empty()) {
      object("ip_address", client_->webhook_ip_address_.empty() ? "<unknown>" : client_->webhook_ip_address_);
    }
    if (client_->allowed_update_types_ != DEFAULT_ALLOWED_UPDATE_TYPES) {
      object("allowed_updates", JsonUpdateTypes(client_->allowed_update_types_));
    }
    if (client_->last_synchronization_error_date_ > 0) {
      object("last_synchronization_error_date", client_->last_synchronization_error_date_);
    }
  }

 private:
  const Client *client_;
};

class Client::JsonStickerSet final : public td::Jsonable {
 public:
  JsonStickerSet(const td_api::stickerSet *sticker_set, const Client *client)
      : sticker_set_(sticker_set), client_(client) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (sticker_set_->id_ == GREAT_MINDS_SET_ID) {
      object("name", GREAT_MINDS_SET_NAME);
    } else {
      object("name", sticker_set_->name_);
    }
    object("title", sticker_set_->title_);
    if (sticker_set_->thumbnail_ != nullptr) {
      client_->json_store_thumbnail(object, sticker_set_->thumbnail_.get());
    }

    auto type = get_sticker_type(sticker_set_->sticker_type_);
    object("sticker_type", type);
    object("contains_masks", td::JsonBool(type == "mask"));

    object("stickers", JsonStickers(sticker_set_->stickers_, client_));
  }

 private:
  const td_api::stickerSet *sticker_set_;
  const Client *client_;
};

class Client::JsonInlineMessageId final : public td::Jsonable {
 public:
  explicit JsonInlineMessageId(const td_api::inlineMessageId *message) : message_(message) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    if (!message_->id_.empty()) {
      object("inline_message_id", message_->id_);
    }
  }

 private:
  const td_api::inlineMessageId *message_;
};

class Client::JsonPreparedInlineMessageId final : public td::Jsonable {
 public:
  explicit JsonPreparedInlineMessageId(const td_api::preparedInlineMessageId *message) : message_(message) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", message_->id_);
    object("expiration_date", message_->expiration_date_);
  }

 private:
  const td_api::preparedInlineMessageId *message_;
};

class Client::JsonPreparedKeyboardButton final : public td::Jsonable {
 public:
  explicit JsonPreparedKeyboardButton(const td_api::text *button) : button_(button) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", button_->text_);
  }

 private:
  const td_api::text *button_;
};

class Client::TdOnOkCallback final : public TdQueryCallback {
 public:
  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ != 401 && error->code_ != 406 && error->code_ != 500) {
        LOG(ERROR) << "Query has failed: " << td::oneline(to_string(error));
      }
    }
  }
};

class Client::TdOnAuthorizationCallback final : public TdQueryCallback {
 public:
  explicit TdOnAuthorizationCallback(Client *client) : client_(client) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    bool was_ready = client_->authorization_state_->get_id() != td_api::authorizationStateWaitPhoneNumber::ID;
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ != 401 && was_ready) {
        // try again
        return client_->on_update_authorization_state();
      }

      client_->log_out(error->code_, error->message_);
    } else if (was_ready) {
      client_->on_update_authorization_state();
    }
  }

 private:
  Client *client_;
};

class Client::TdOnInitCallback final : public TdQueryCallback {
 public:
  explicit TdOnInitCallback(Client *client) : client_(client) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      LOG(WARNING) << "Failed to initialize due to " << td::oneline(to_string(result));
      client_->close();
    }
  }

 private:
  Client *client_;
};

class Client::TdOnGetUserProfilePhotosCallback final : public TdQueryCallback {
 public:
  TdOnGetUserProfilePhotosCallback(const Client *client, PromisedQueryPtr query)
      : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::chatPhotos::ID);
    auto profile_photos = move_object_as<td_api::chatPhotos>(result);
    answer_query(JsonChatPhotos(profile_photos.get(), client_), std::move(query_));
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetUserProfileAudiosCallback final : public TdQueryCallback {
 public:
  TdOnGetUserProfileAudiosCallback(const Client *client, PromisedQueryPtr query)
      : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::audios::ID);
    auto audios = move_object_as<td_api::audios>(result);
    answer_query(JsonAudios(audios.get(), client_), std::move(query_));
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnSendMessageCallback final : public TdQueryCallback {
 public:
  TdOnSendMessageCallback(Client *client, int64 chat_id, PromisedQueryPtr query)
      : client_(client), chat_id_(chat_id), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      client_->decrease_yet_unsent_message_count(chat_id_, 1);
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::message::ID);
    auto query_id = client_->get_send_message_query_id(std::move(query_), false);
    client_->on_sent_message(move_object_as<td_api::message>(result), query_id);
  }

 private:
  Client *client_;
  int64 chat_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnReturnBusinessMessageCallback final : public TdQueryCallback {
 public:
  TdOnReturnBusinessMessageCallback(Client *client, td::string business_connection_id, PromisedQueryPtr query)
      : client_(client), business_connection_id_(std::move(business_connection_id)), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::businessMessage::ID);
    auto message = client_->create_business_message(std::move(business_connection_id_),
                                                    move_object_as<td_api::businessMessage>(result));
    answer_query(JsonMessage(message.get(), true, "business message", client_), std::move(query_));
  }

 private:
  Client *client_;
  td::string business_connection_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnSendMessageAlbumCallback final : public TdQueryCallback {
 public:
  TdOnSendMessageAlbumCallback(Client *client, int64 chat_id, std::size_t message_count, PromisedQueryPtr query)
      : client_(client), chat_id_(chat_id), message_count_(message_count), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      if (message_count_ > 0) {
        client_->decrease_yet_unsent_message_count(chat_id_, static_cast<int32>(message_count_));
      }
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::messages::ID);
    auto messages = move_object_as<td_api::messages>(result);
    CHECK(messages->messages_.size() == message_count_);
    auto query_id = client_->get_send_message_query_id(std::move(query_), true);
    for (auto &message : messages->messages_) {
      client_->on_sent_message(std::move(message), query_id);
    }
  }

 private:
  Client *client_;
  int64 chat_id_;
  std::size_t message_count_;
  PromisedQueryPtr query_;
};

class Client::TdOnSendBusinessMessageAlbumCallback final : public TdQueryCallback {
 public:
  TdOnSendBusinessMessageAlbumCallback(Client *client, td::string business_connection_id, PromisedQueryPtr query)
      : client_(client), business_connection_id_(std::move(business_connection_id)), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::businessMessages::ID);
    auto messages = move_object_as<td_api::businessMessages>(result);
    td::vector<td::string> message_strings;
    for (auto &message : messages->messages_) {
      auto message_info = client_->create_business_message(business_connection_id_, std::move(message));
      message_strings.push_back(
          td::json_encode<td::string>(JsonMessage(message_info.get(), true, "sent business message", client_)));
    }
    answer_query(JsonMessages(message_strings), std::move(query_));
  }

 private:
  Client *client_;
  td::string business_connection_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnForwardMessagesCallback final : public TdQueryCallback {
 public:
  TdOnForwardMessagesCallback(Client *client, int64 chat_id, std::size_t message_count, PromisedQueryPtr query)
      : client_(client), chat_id_(chat_id), message_count_(message_count), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      if (message_count_ > 0) {
        client_->decrease_yet_unsent_message_count(chat_id_, static_cast<int32>(message_count_));
      }
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::messages::ID);
    auto messages = move_object_as<td_api::messages>(result);
    CHECK(messages->messages_.size() == message_count_);
    td::remove_if(messages->messages_, [](const auto &message) { return message == nullptr; });
    if (messages->messages_.size() != message_count_) {
      client_->decrease_yet_unsent_message_count(chat_id_,
                                                 static_cast<int32>(message_count_ - messages->messages_.size()));
    }
    if (messages->messages_.empty()) {
      return fail_query_with_error(std::move(query_), 400, "Messages can't be forwarded");
    }
    auto query_id = client_->get_send_message_query_id(std::move(query_), true);
    for (auto &message : messages->messages_) {
      client_->on_sent_message(std::move(message), query_id);
    }
  }

 private:
  Client *client_;
  int64 chat_id_;
  std::size_t message_count_;
  PromisedQueryPtr query_;
};

class Client::TdOnDeleteFailedToSendMessageCallback final : public TdQueryCallback {
 public:
  TdOnDeleteFailedToSendMessageCallback(Client *client, int64 chat_id, int64 message_id)
      : client_(client)
      , chat_id_(chat_id)
      , message_id_(message_id)
      , old_chat_description_(client->get_chat_description(chat_id)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ != 401 && !client_->need_close_ && !client_->closing_ && !client_->logging_out_ &&
          client_->have_message_access(chat_id_)) {
        LOG(ERROR) << "Can't delete failed to send message " << message_id_ << " because of "
                   << td::oneline(to_string(error)) << " in " << client_->get_chat_description(chat_id_)
                   << ". Old chat description: " << old_chat_description_;
      }
      return;
    }

    CHECK(result->get_id() == td_api::ok::ID);
    if (client_->get_message(chat_id_, message_id_, true) != nullptr) {
      LOG(ERROR) << "Have cache for message " << message_id_ << " in the chat " << chat_id_;
      client_->delete_message(chat_id_, message_id_, false);
    }
  }

 private:
  Client *client_;
  int64 chat_id_;
  int64 message_id_;
  td::string old_chat_description_;
};

class Client::TdOnEditMessageCallback final : public TdQueryCallback {
 public:
  TdOnEditMessageCallback(const Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::message::ID);
    auto message = move_object_as<td_api::message>(result);
    int64 chat_id = message->chat_id_;
    int64 message_id = message->id_;

    auto message_info = client_->get_message(chat_id, message_id, true);
    if (message_info == nullptr) {
      return fail_query_with_error(std::move(query_), 400, "message not found");
    }
    message_info->is_content_changed = false;
    answer_query(JsonMessage(message_info, false, "edited message", client_), std::move(query_));
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnEditInlineMessageCallback final : public TdQueryCallback {
 public:
  explicit TdOnEditInlineMessageCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::ok::ID);
    answer_query(td::JsonTrue(), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnStopPollCallback final : public TdQueryCallback {
 public:
  TdOnStopPollCallback(const Client *client, int64 chat_id, int64 message_id, PromisedQueryPtr query)
      : client_(client), chat_id_(chat_id), message_id_(message_id), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::ok::ID);
    auto message_info = client_->get_message(chat_id_, message_id_, true);
    if (message_info == nullptr) {
      return fail_query_with_error(std::move(query_), 400, "message not found");
    }
    if (message_info->content->get_id() != td_api::messagePoll::ID) {
      LOG(ERROR) << "Poll not found in " << message_id_ << " in " << chat_id_;
      return fail_query_with_error(std::move(query_), 400, "message poll not found");
    }
    auto message_poll = static_cast<const td_api::messagePoll *>(message_info->content.get());
    answer_query(
        JsonPoll(message_poll->poll_.get(), message_poll->description_.get(), message_poll->media_.get(), client_),
        std::move(query_));
  }

 private:
  const Client *client_;
  int64 chat_id_;
  int64 message_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnStopBusinessPollCallback final : public TdQueryCallback {
 public:
  TdOnStopBusinessPollCallback(Client *client, const td::string &business_connection_id, PromisedQueryPtr query)
      : client_(client), business_connection_id_(business_connection_id), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::businessMessage::ID);
    auto message = client_->create_business_message(std::move(business_connection_id_),
                                                    move_object_as<td_api::businessMessage>(result));
    if (message->content->get_id() != td_api::messagePoll::ID) {
      LOG(ERROR) << "Poll not found in a business message from connection " << business_connection_id_;
      return fail_query_with_error(std::move(query_), 400, "message poll not found");
    }
    auto message_poll = static_cast<const td_api::messagePoll *>(message->content.get());
    answer_query(
        JsonPoll(message_poll->poll_.get(), message_poll->description_.get(), message_poll->media_.get(), client_),
        std::move(query_));
  }

 private:
  Client *client_;
  td::string business_connection_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnPostStoryCallback final : public TdQueryCallback {
 public:
  TdOnPostStoryCallback(Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::story::ID);
    client_->on_sent_story(move_object_as<td_api::story>(result), std::move(query_));
  }

 private:
  Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnRepostStoryCallback final : public TdQueryCallback {
 public:
  TdOnRepostStoryCallback(Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::story::ID);
    auto story = move_object_as<td_api::story>(result);
    answer_query(JsonStory(story->poster_chat_id_, story->id_, client_), std::move(query_));
  }

 private:
  Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetStoryCallback final : public TdQueryCallback {
 public:
  TdOnGetStoryCallback(Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::story::ID);
    auto story = static_cast<const td_api::story *>(result.get());
    answer_query(JsonStory(story->poster_chat_id_, story->id_, client_), std::move(query_));
  }

 private:
  Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnOkQueryCallback final : public TdQueryCallback {
 public:
  explicit TdOnOkQueryCallback(PromisedQueryPtr query) : query_(std::move(query)) {
    CHECK(query_ != nullptr);
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::ok::ID);
    answer_query(td::JsonTrue(), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

template <class OnSuccess>
class Client::TdOnCheckUserCallback final : public TdQueryCallback {
 public:
  TdOnCheckUserCallback(const Client *client, PromisedQueryPtr query, OnSuccess on_success)
      : client_(client), query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result), "user not found");
    }

    CHECK(result->get_id() == td_api::user::ID);
    auto user = move_object_as<td_api::user>(result);
    auto user_info = client_->get_user_info(user->id_);
    CHECK(user_info != nullptr);  // it must have already been received through updates

    client_->check_user_read_access(user_info, std::move(query_), std::move(on_success_));
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnCheckUserNoFailCallback final : public TdQueryCallback {
 public:
  TdOnCheckUserNoFailCallback(PromisedQueryPtr query, OnSuccess on_success)
      : query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    on_success_(std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnCheckChatCallback final : public TdQueryCallback {
 public:
  TdOnCheckChatCallback(const Client *client, bool by_username, AccessRights access_rights, PromisedQueryPtr query,
                        OnSuccess on_success)
      : client_(client)
      , by_username_(by_username)
      , access_rights_(access_rights)
      , query_(std::move(query))
      , on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result), "chat not found");
    }

    CHECK(result->get_id() == td_api::chat::ID);
    auto chat = move_object_as<td_api::chat>(result);
    auto chat_info = client_->get_chat(chat->id_);
    CHECK(chat_info != nullptr);  // it must have already been received through updates
    CHECK(chat_info->title == chat->title_);
    if (by_username_) {
      bool is_allowed = false;
      switch (chat_info->type) {
        case ChatInfo::Type::Private: {
          auto user_info = client_->get_user_info(chat_info->user_id);
          CHECK(user_info != nullptr);
          is_allowed = (user_info->type == UserInfo::Type::Bot);
          break;
        }
        case ChatInfo::Type::Supergroup:
          is_allowed = true;
          break;
        default:
          // not allowed
          break;
      }
      if (!is_allowed) {
        return fail_query(400, "Bad Request: chat not found", std::move(query_));
      }
    }

    client_->check_chat_access(chat->id_, access_rights_, chat_info, std::move(query_), std::move(on_success_));
  }

 private:
  const Client *client_;
  bool by_username_;
  AccessRights access_rights_;
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnCheckChatNoFailCallback final : public TdQueryCallback {
 public:
  TdOnCheckChatNoFailCallback(int64 chat_id, PromisedQueryPtr query, OnSuccess on_success)
      : chat_id_(chat_id), query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    on_success_(chat_id_, std::move(query_));
  }

 private:
  int64 chat_id_;
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnCheckBusinessConnectionCallback final : public TdQueryCallback {
 public:
  TdOnCheckBusinessConnectionCallback(Client *client, PromisedQueryPtr query, OnSuccess on_success)
      : client_(client), query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result),
                                   "business connection not found");
    }

    CHECK(result->get_id() == td_api::businessConnection::ID);
    auto connection = client_->add_business_connection(move_object_as<td_api::businessConnection>(result), false);
    on_success_(connection, std::move(query_));
  }

 private:
  Client *client_;
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnSearchStickerSetCallback final : public TdQueryCallback {
 public:
  TdOnSearchStickerSetCallback(PromisedQueryPtr query, OnSuccess on_success)
      : query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result), "sticker set not found");
    }

    CHECK(result->get_id() == td_api::stickerSet::ID);
    auto sticker_set = move_object_as<td_api::stickerSet>(result);
    on_success_(sticker_set->id_, std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

class Client::TdOnResolveBotUsernameCallback final : public TdQueryCallback {
 public:
  TdOnResolveBotUsernameCallback(Client *client, td::string username)
      : client_(client), username_(std::move(username)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return client_->on_resolve_bot_username(username_, 0);
    }

    CHECK(result->get_id() == td_api::chat::ID);
    auto chat = move_object_as<td_api::chat>(result);
    auto chat_info = client_->get_chat(chat->id_);
    CHECK(chat_info != nullptr);  // it must have already been received through updates
    if (chat_info->type != ChatInfo::Type::Private) {
      return client_->on_resolve_bot_username(username_, 0);
    }
    auto user_info = client_->get_user_info(chat_info->user_id);
    CHECK(user_info != nullptr);
    if (user_info->type != UserInfo::Type::Bot) {
      return client_->on_resolve_bot_username(username_, 0);
    }

    client_->on_resolve_bot_username(username_, chat_info->user_id);
  }

 private:
  Client *client_;
  td::string username_;
};

template <class OnSuccess>
class Client::TdOnCheckMessageCallback final : public TdQueryCallback {
 public:
  TdOnCheckMessageCallback(Client *client, int64 chat_id, int64 message_id, bool allow_empty, td::Slice message_type,
                           PromisedQueryPtr query, OnSuccess on_success)
      : client_(client)
      , chat_id_(chat_id)
      , message_id_(message_id)
      , allow_empty_(allow_empty)
      , message_type_(message_type)
      , query_(std::move(query))
      , on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ == 429) {
        LOG(WARNING) << "Failed to get message " << message_id_ << " in " << chat_id_ << ": " << message_type_;
      }
      if (allow_empty_) {
        return on_success_(chat_id_, 0, std::move(query_));
      }
      return fail_query_with_error(std::move(query_), std::move(error), PSLICE() << message_type_ << " not found");
    }

    CHECK(result->get_id() == td_api::message::ID);
    const auto *message_info = client_->add_message(move_object_as<td_api::message>(result));
    CHECK(message_info->chat_id == chat_id_);
    CHECK(message_info->id == message_id_);
    on_success_(chat_id_, message_id_, std::move(query_));
  }

 private:
  Client *client_;
  int64 chat_id_;
  int64 message_id_;
  bool allow_empty_;
  td::Slice message_type_;
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnCheckMessagesCallback final : public TdQueryCallback {
 public:
  TdOnCheckMessagesCallback(Client *client, int64 chat_id, bool allow_empty, td::Slice message_type,
                            PromisedQueryPtr query, OnSuccess on_success)
      : client_(client)
      , chat_id_(chat_id)
      , allow_empty_(allow_empty)
      , message_type_(message_type)
      , query_(std::move(query))
      , on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ == 429) {
        LOG(WARNING) << "Failed to get messages in " << chat_id_ << ": " << message_type_;
      }
      if (allow_empty_) {
        return on_success_(chat_id_, td::vector<int64>(), std::move(query_));
      }
      return fail_query_with_error(std::move(query_), std::move(error), PSLICE() << message_type_ << " not found");
    }

    CHECK(result->get_id() == td_api::messages::ID);
    auto messages = move_object_as<td_api::messages>(result);
    td::vector<int64> message_ids;
    for (auto &message : messages->messages_) {
      if (message == nullptr) {
        if (!allow_empty_) {
          return fail_query_with_error(std::move(query_), 400, PSLICE() << message_type_ << " not found");
        }
        continue;
      }
      const auto *message_info = client_->add_message(std::move(message));
      CHECK(message_info->chat_id == chat_id_);
      message_ids.push_back(message_info->id);
    }
    on_success_(chat_id_, std::move(message_ids), std::move(query_));
  }

 private:
  Client *client_;
  int64 chat_id_;
  bool allow_empty_;
  td::Slice message_type_;
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnCheckForumTopicCallback final : public TdQueryCallback {
 public:
  TdOnCheckForumTopicCallback(Client *client, int64 chat_id, int32 forum_topic_id, PromisedQueryPtr query,
                              OnSuccess on_success)
      : client_(client)
      , chat_id_(chat_id)
      , forum_topic_id_(forum_topic_id)
      , query_(std::move(query))
      , on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ == 429) {
        LOG(WARNING) << "Failed to get forum topic " << forum_topic_id_ << " in " << chat_id_;
      }
      return fail_query_with_error(std::move(query_), std::move(error), "Message thread not found");
    }

    CHECK(result->get_id() == td_api::forumTopic::ID);
    auto forum_topic = move_object_as<td_api::forumTopic>(result);
    CHECK(forum_topic->info_->chat_id_ == chat_id_);
    CHECK(forum_topic->info_->forum_topic_id_ == forum_topic_id_);
    auto message_id = as_tdlib_message_id(forum_topic_id_);
    if (client_->get_chat_type(chat_id_) == ChatType::Supergroup &&
        client_->get_message(chat_id_, message_id, false) == nullptr) {
      auto on_check_message = [forum_topic_id = forum_topic_id_, on_success = std::move(on_success_)](
                                  int64 chat_id, int64 message_id, PromisedQueryPtr query) mutable {
        on_success(chat_id, make_object<td_api::messageTopicForum>(forum_topic_id), std::move(query));
      };
      return client_->send_request(make_object<td_api::getMessage>(chat_id_, message_id),
                                   td::make_unique<TdOnCheckMessageCallback<decltype(on_check_message)>>(
                                       client_, chat_id_, message_id, true, "top forum topic message",
                                       std::move(query_), std::move(on_check_message)));
    }
    on_success_(chat_id_, make_object<td_api::messageTopicForum>(forum_topic_id_), std::move(query_));
  }

 private:
  Client *client_;
  int64 chat_id_;
  int32 forum_topic_id_;
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnCheckRemoteFileIdCallback final : public TdQueryCallback {
 public:
  TdOnCheckRemoteFileIdCallback(PromisedQueryPtr query, OnSuccess on_success)
      : query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result), "invalid file_id");
    }

    CHECK(result->get_id() == td_api::file::ID);
    on_success_(move_object_as<td_api::file>(result), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

template <class OnSuccess>
class Client::TdOnGetChatMemberCallback final : public TdQueryCallback {
 public:
  TdOnGetChatMemberCallback(PromisedQueryPtr query, OnSuccess on_success)
      : query_(std::move(query)), on_success_(std::move(on_success)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::chatMember::ID);
    on_success_(move_object_as<td_api::chatMember>(result), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
  OnSuccess on_success_;
};

class Client::TdOnDownloadFileCallback final : public TdQueryCallback {
 public:
  TdOnDownloadFileCallback(Client *client, int32 file_id) : client_(client), file_id_(file_id) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      return client_->on_file_download(file_id_, td::Status::Error(error->code_, error->message_));
    }
    CHECK(result->get_id() == td_api::file::ID);
    if (client_->is_file_being_downloaded(file_id_)) {  // if download is yet not finished
      client_->download_started_file_ids_.insert(file_id_);
    }
    client_->on_update_file(move_object_as<td_api::file>(result));
  }

 private:
  Client *client_;
  int32 file_id_;
};

class Client::TdOnCancelDownloadFileCallback final : public TdQueryCallback {
 public:
  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      LOG(ERROR) << "Failed to cancel download file";
      return;
    }
    CHECK(result->get_id() == td_api::ok::ID);
  }
};

class Client::TdOnGetReplyMessageCallback final : public TdQueryCallback {
 public:
  TdOnGetReplyMessageCallback(Client *client, int64 chat_id) : client_(client), chat_id_(chat_id) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return client_->on_get_reply_message(chat_id_, nullptr);
    }

    CHECK(result->get_id() == td_api::message::ID);
    client_->on_get_reply_message(chat_id_, move_object_as<td_api::message>(result));
  }

 private:
  Client *client_;
  int64 chat_id_;
};

class Client::TdOnGetEditedMessageCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetEditedMessageCallback(Client *client) : client_(client) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ == 429) {
        LOG(WARNING) << "Failed to get edited message";
      }
      return client_->on_get_edited_message(nullptr);
    }

    CHECK(result->get_id() == td_api::message::ID);
    client_->on_get_edited_message(move_object_as<td_api::message>(result));
  }

 private:
  Client *client_;
};

class Client::TdOnGetCallbackQueryMessageCallback final : public TdQueryCallback {
 public:
  TdOnGetCallbackQueryMessageCallback(Client *client, int64 user_id, int state)
      : client_(client), user_id_(user_id), state_(state) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->code_ == 429) {
        LOG(WARNING) << "Failed to get callback query message";
      }
      return client_->on_get_callback_query_message(nullptr, user_id_, state_);
    }

    CHECK(result->get_id() == td_api::message::ID);
    client_->on_get_callback_query_message(move_object_as<td_api::message>(result), user_id_, state_);
  }

 private:
  Client *client_;
  int64 user_id_;
  int state_;
};

class Client::TdOnGetChatPinnedMessageCallback final : public TdQueryCallback {
 public:
  TdOnGetChatPinnedMessageCallback(Client *client, int64 chat_id, PromisedQueryPtr query)
      : client_(client), chat_id_(chat_id), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    int64 pinned_message_id = 0;
    td::vector<int64> sticker_set_ids;
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (is_special_error_code(error->code_)) {
        return fail_query_with_error(std::move(query_), std::move(error));
      } else if (error->code_ != 404 && error->message_ != "CHANNEL_PRIVATE") {
        LOG(ERROR) << "Failed to get chat pinned message: " << to_string(error);
      }
    } else {
      CHECK(result->get_id() == td_api::message::ID);
      const auto *message_info = client_->add_message(move_object_as<td_api::message>(result));
      pinned_message_id = message_info->id;
      CHECK(message_info->chat_id == chat_id_);
      td::append(sticker_set_ids, get_message_sticker_set_ids(message_info));
    }

    auto chat_info = client_->get_chat(chat_id_);
    CHECK(chat_info != nullptr);
    if (chat_info->type == ChatInfo::Type::Supergroup) {
      auto supergroup_info = client_->get_supergroup_info(chat_info->supergroup_id);
      CHECK(supergroup_info != nullptr);
      sticker_set_ids.push_back(supergroup_info->sticker_set_id);
      sticker_set_ids.push_back(supergroup_info->custom_emoji_sticker_set_id);
    } else if (chat_info->type == ChatInfo::Type::Private) {
      auto user_info = client_->get_user_info(chat_info->user_id);
      CHECK(user_info != nullptr);
      if (user_info->business_info != nullptr && user_info->business_info->start_page_ != nullptr &&
          user_info->business_info->start_page_->sticker_ != nullptr) {
        sticker_set_ids.push_back(user_info->business_info->start_page_->sticker_->set_id_);
      }
    }
    client_->get_sticker_set_names(
        std::move(sticker_set_ids),
        td::PromiseCreator::lambda([actor_id = client_->actor_id(client_), chat_id = chat_id_, pinned_message_id,
                                    query = std::move(query_)](td::Unit) mutable {
          send_closure(actor_id, &Client::return_chat_full_info, chat_id, pinned_message_id, std::move(query));
        }));
  }

 private:
  Client *client_;
  int64 chat_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetChatPinnedMessageToUnpinCallback final : public TdQueryCallback {
 public:
  TdOnGetChatPinnedMessageToUnpinCallback(Client *client, int64 chat_id, PromisedQueryPtr query)
      : client_(client), chat_id_(chat_id), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (is_special_error_code(error->code_)) {
        return fail_query_with_error(std::move(query_), std::move(error));
      } else {
        return fail_query_with_error(std::move(query_), 400, "Message to unpin not found");
      }
    }

    CHECK(result->get_id() == td_api::message::ID);
    const auto *message_info = client_->add_message(move_object_as<td_api::message>(result));
    CHECK(message_info->chat_id == chat_id_);
    client_->send_request(make_object<td_api::unpinChatMessage>(chat_id_, message_info->id),
                          td::make_unique<TdOnOkQueryCallback>(std::move(query_)));
  }

 private:
  Client *client_;
  int64 chat_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetMyCommandsCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetMyCommandsCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::botCommands::ID);
    auto bot_commands = move_object_as<td_api::botCommands>(result);
    answer_query(td::json_array(bot_commands->commands_, [](auto &command) { return JsonBotCommand(command.get()); }),
                 std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetMyDefaultAdministratorRightsCallback final : public TdQueryCallback {
 public:
  TdOnGetMyDefaultAdministratorRightsCallback(bool for_channels, PromisedQueryPtr query)
      : for_channels_(for_channels), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::userFullInfo::ID);
    auto full_info = move_object_as<td_api::userFullInfo>(result);
    if (full_info->bot_info_ == nullptr) {
      LOG(ERROR) << "Have no bot info for self";
      return fail_query_with_error(std::move(query_), 500, "Requested data is inaccessible");
    }
    auto bot_info = std::move(full_info->bot_info_);
    const auto *rights = for_channels_ ? bot_info->default_channel_administrator_rights_.get()
                                       : bot_info->default_group_administrator_rights_.get();
    answer_query(JsonChatAdministratorRights(rights, for_channels_ ? ChatType::Channel : ChatType::Supergroup),
                 std::move(query_));
  }

 private:
  bool for_channels_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetMyNameCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetMyNameCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::text::ID);
    auto text = move_object_as<td_api::text>(result);
    answer_query(JsonBotName(text.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetMyDescriptionCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetMyDescriptionCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::text::ID);
    auto text = move_object_as<td_api::text>(result);
    answer_query(JsonBotInfoDescription(text.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetMyShortDescriptionCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetMyShortDescriptionCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::text::ID);
    auto text = move_object_as<td_api::text>(result);
    answer_query(JsonBotInfoShortDescription(text.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetForumTopicInfoCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetForumTopicInfoCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::forumTopicInfo::ID);
    auto forum_topic_info = move_object_as<td_api::forumTopicInfo>(result);
    answer_query(JsonForumTopicInfo(forum_topic_info.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetMessagesCallback final : public TdQueryCallback {
 public:
  TdOnGetMessagesCallback(Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::messages::ID);
    auto messages = move_object_as<td_api::messages>(result);
    td::vector<td::string> message_strings;
    for (auto &message : messages->messages_) {
      auto message_info = client_->create_message(std::move(message));
      message_strings.push_back(
          td::json_encode<td::string>(JsonMessage(message_info.get(), false, "TdOnGetMessagesCallback", client_)));
    }
    answer_query(JsonMessages(message_strings), std::move(query_));
  }

 private:
  Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetMenuButtonCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetMenuButtonCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::botMenuButton::ID);
    auto menu_button = move_object_as<td_api::botMenuButton>(result);
    answer_query(JsonBotMenuButton(menu_button.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetChatFullInfoCallback final : public TdQueryCallback {
 public:
  TdOnGetChatFullInfoCallback(Client *client, int64 chat_id, PromisedQueryPtr query)
      : client_(client), chat_id_(chat_id), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    // we don't need the result, everything is already received through updates

    client_->send_request(make_object<td_api::getChatPinnedMessage>(chat_id_),
                          td::make_unique<TdOnGetChatPinnedMessageCallback>(client_, chat_id_, std::move(query_)));
  }

 private:
  Client *client_;
  int64 chat_id_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetGroupMembersCallback final : public TdQueryCallback {
 public:
  TdOnGetGroupMembersCallback(const Client *client, bool administrators_only, PromisedQueryPtr query)
      : client_(client), administrators_only_(administrators_only), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::basicGroupFullInfo::ID);
    auto group_full_info = move_object_as<td_api::basicGroupFullInfo>(result);
    auto return_bots = to_bool(query_->arg("return_bots"));
    answer_query(
        JsonChatMembers(group_full_info->members_, ChatType::Group, administrators_only_, return_bots, client_),
        std::move(query_));
  }

 private:
  const Client *client_;
  bool administrators_only_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetSupergroupMembersCallback final : public TdQueryCallback {
 public:
  TdOnGetSupergroupMembersCallback(const Client *client, ChatType chat_type, PromisedQueryPtr query)
      : client_(client), chat_type_(chat_type), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::chatMembers::ID);
    auto chat_members = move_object_as<td_api::chatMembers>(result);
    auto return_bots = to_bool(query_->arg("return_bots"));
    answer_query(JsonChatMembers(chat_members->members_, chat_type_, false, return_bots, client_), std::move(query_));
  }

 private:
  const Client *client_;
  ChatType chat_type_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetSupergroupMemberCountCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetSupergroupMemberCountCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::supergroupFullInfo::ID);
    auto supergroup_full_info = move_object_as<td_api::supergroupFullInfo>(result);
    answer_query(td::VirtuallyJsonableInt(supergroup_full_info->member_count_), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetBotTokenCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetBotTokenCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::text::ID);
    auto text = move_object_as<td_api::text>(result);
    answer_query(td::VirtuallyJsonableString(text->text_), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetBotAccessSettingsCallback final : public TdQueryCallback {
 public:
  TdOnGetBotAccessSettingsCallback(const Client *client, PromisedQueryPtr query)
      : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::botAccessSettings::ID);
    auto settings = move_object_as<td_api::botAccessSettings>(result);
    answer_query(JsonBotAccessSettings(settings.get(), client_), std::move(query_));
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnCreateInvoiceLinkCallback final : public TdQueryCallback {
 public:
  explicit TdOnCreateInvoiceLinkCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::httpUrl::ID);
    auto http_url = move_object_as<td_api::httpUrl>(result);
    answer_query(td::VirtuallyJsonableString(http_url->url_), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnReplacePrimaryChatInviteLinkCallback final : public TdQueryCallback {
 public:
  explicit TdOnReplacePrimaryChatInviteLinkCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::chatInviteLink::ID);
    auto invite_link = move_object_as<td_api::chatInviteLink>(result);
    answer_query(td::VirtuallyJsonableString(invite_link->invite_link_), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetChatInviteLinkCallback final : public TdQueryCallback {
 public:
  TdOnGetChatInviteLinkCallback(const Client *client, PromisedQueryPtr query)
      : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    if (result->get_id() == td_api::chatInviteLink::ID) {
      auto invite_link = move_object_as<td_api::chatInviteLink>(result);
      answer_query(JsonChatInviteLink(invite_link.get(), client_), std::move(query_));
    } else {
      CHECK(result->get_id() == td_api::chatInviteLinks::ID);
      auto invite_links = move_object_as<td_api::chatInviteLinks>(result);
      CHECK(!invite_links->invite_links_.empty());
      answer_query(JsonChatInviteLink(invite_links->invite_links_[0].get(), client_), std::move(query_));
    }
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnUpgradeGiftCallback final : public TdQueryCallback {
 public:
  explicit TdOnUpgradeGiftCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    answer_query(td::JsonTrue(), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetStarAmountCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetStarAmountCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::starAmount::ID);
    auto amount = move_object_as<td_api::starAmount>(result);
    answer_query(JsonStarAmount(amount.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetReceivedGiftsCallback final : public TdQueryCallback {
 public:
  TdOnGetReceivedGiftsCallback(Client *client, bool can_be_managed, PromisedQueryPtr query)
      : client_(client), can_be_managed_(can_be_managed), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::receivedGifts::ID);
    auto gifts = move_object_as<td_api::receivedGifts>(result);
    td::vector<int64> sticker_set_ids;
    for (const auto &gift : gifts->gifts_) {
      td::combine(sticker_set_ids, get_sent_gift_sticker_set_ids(gift->gift_));
    }
    client_->get_sticker_set_names(
        std::move(sticker_set_ids),
        td::PromiseCreator::lambda([actor_id = client_->actor_id(client_), gifts = std::move(gifts),
                                    can_be_managed = can_be_managed_, query = std::move(query_)](td::Unit) mutable {
          send_closure(actor_id, &Client::return_received_gifts, std::move(gifts), can_be_managed, std::move(query));
        }));
  }

 private:
  Client *client_;
  bool can_be_managed_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetMyStarBalanceCallback final : public TdQueryCallback {
 public:
  explicit TdOnGetMyStarBalanceCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::starTransactions::ID);
    auto transactions = move_object_as<td_api::starTransactions>(result);
    answer_query(JsonStarAmount(transactions->star_amount_.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetStarTransactionsCallback final : public TdQueryCallback {
 public:
  TdOnGetStarTransactionsCallback(Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::starTransactions::ID);
    auto transactions = move_object_as<td_api::starTransactions>(result);
    td::vector<int64> sticker_set_ids;
    for (const auto &transaction : transactions->transactions_) {
      if (transaction->type_->get_id() == td_api::starTransactionTypeGiftPurchase::ID) {
        td::combine(sticker_set_ids,
                    get_gift_sticker_set_ids(
                        static_cast<const td_api::starTransactionTypeGiftPurchase *>(transaction->type_.get())->gift_));
      }
    }
    client_->get_sticker_set_names(
        std::move(sticker_set_ids),
        td::PromiseCreator::lambda([actor_id = client_->actor_id(client_), transactions = std::move(transactions),
                                    query = std::move(query_)](td::Unit) mutable {
          send_closure(actor_id, &Client::return_star_transactions, std::move(transactions), std::move(query));
        }));
  }

 private:
  Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetGameHighScoresCallback final : public TdQueryCallback {
 public:
  TdOnGetGameHighScoresCallback(const Client *client, PromisedQueryPtr query)
      : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::gameHighScores::ID);
    auto game_high_scores = move_object_as<td_api::gameHighScores>(result);
    answer_query(td::json_array(game_high_scores->scores_,
                                [client = client_](auto &score) { return JsonGameHighScore(score.get(), client); }),
                 std::move(query_));
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnAnswerWebAppQueryCallback final : public TdQueryCallback {
 public:
  explicit TdOnAnswerWebAppQueryCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::inlineMessageId::ID);
    auto message = move_object_as<td_api::inlineMessageId>(result);
    answer_query(JsonInlineMessageId(message.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnAnswerGuestQueryCallback final : public TdQueryCallback {
 public:
  explicit TdOnAnswerGuestQueryCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::inlineMessageId::ID);
    auto inline_message_id = move_object_as<td_api::inlineMessageId>(result);
    answer_query(JsonInlineMessageId(inline_message_id.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnSavePreparedInlineMessageCallback final : public TdQueryCallback {
 public:
  explicit TdOnSavePreparedInlineMessageCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::preparedInlineMessageId::ID);
    auto message = move_object_as<td_api::preparedInlineMessageId>(result);
    answer_query(JsonPreparedInlineMessageId(message.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnSavePreparedKeyboardButtonCallback final : public TdQueryCallback {
 public:
  explicit TdOnSavePreparedKeyboardButtonCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::text::ID);
    auto text = move_object_as<td_api::text>(result);
    answer_query(JsonPreparedKeyboardButton(text.get()), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

class Client::TdOnGetUserChatBoostsCallback final : public TdQueryCallback {
 public:
  TdOnGetUserChatBoostsCallback(Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::foundChatBoosts::ID);
    auto chat_boosts = move_object_as<td_api::foundChatBoosts>(result);
    answer_query(JsonChatBoosts(chat_boosts.get(), client_), std::move(query_));
  }

 private:
  Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetGiftsCallback final : public TdQueryCallback {
 public:
  TdOnGetGiftsCallback(Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::availableGifts::ID);
    auto available_gifts = move_object_as<td_api::availableGifts>(result);
    auto gifts = td::transform(std::move(available_gifts->gifts_), [](auto &&gift) { return std::move(gift->gift_); });
    td::vector<int64> sticker_set_ids;
    for (const auto &gift : gifts) {
      td::combine(sticker_set_ids, get_gift_sticker_set_ids(gift));
    }
    client_->get_sticker_set_names(
        std::move(sticker_set_ids),
        td::PromiseCreator::lambda([actor_id = client_->actor_id(client_), gifts = std::move(gifts),
                                    query = std::move(query_)](td::Unit) mutable {
          send_closure(actor_id, &Client::return_gifts, std::move(gifts), std::move(query));
        }));
  }

 private:
  Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnReturnFileCallback final : public TdQueryCallback {
 public:
  TdOnReturnFileCallback(const Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::file::ID);
    auto file = move_object_as<td_api::file>(result);
    answer_query(JsonFile(file.get(), client_, false), std::move(query_));
  }

 private:
  const Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnReturnStickerSetCallback final : public TdQueryCallback {
 public:
  TdOnReturnStickerSetCallback(Client *client, bool return_sticker_set, PromisedQueryPtr query)
      : client_(client), return_sticker_set_(return_sticker_set), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::stickerSet::ID);
    auto sticker_set = move_object_as<td_api::stickerSet>(result);
    client_->on_get_sticker_set_name(sticker_set->id_, sticker_set->name_);
    if (return_sticker_set_) {
      answer_query(JsonStickerSet(sticker_set.get(), client_), std::move(query_));
    } else {
      answer_query(td::JsonTrue(), std::move(query_));
    }
  }

 private:
  Client *client_;
  bool return_sticker_set_;
  PromisedQueryPtr query_;
};

class Client::TdOnGetStickerSetNameCallback final : public TdQueryCallback {
 public:
  TdOnGetStickerSetNameCallback(Client *client, int64 sticker_set_id, td::Promise<td::Unit> &&promise)
      : client_(client), sticker_set_id_(sticker_set_id), promise_(std::move(promise)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(result);
      if (error->message_ == "STICKERSET_INVALID") {
        client_->on_get_sticker_set_name(sticker_set_id_, td::string());
        return promise_.set_value(td::Unit());
      }
      if (error->code_ != 401 && error->code_ != 500) {
        LOG(ERROR) << "Failed to get name of sticker set " << sticker_set_id_ << ": " << td::oneline(to_string(error));
      }
      return promise_.set_error(td::Status::Error(error->code_, error->message_));
    }

    CHECK(result->get_id() == td_api::text::ID);
    auto text = move_object_as<td_api::text>(result);
    client_->on_get_sticker_set_name(sticker_set_id_, text->text_);
    promise_.set_value(td::Unit());
  }

 private:
  Client *client_;
  int64 sticker_set_id_;
  td::Promise<td::Unit> promise_;
};

class Client::TdOnGetStickersCallback final : public TdQueryCallback {
 public:
  TdOnGetStickersCallback(Client *client, PromisedQueryPtr query) : client_(client), query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::stickers::ID);
    auto stickers = move_object_as<td_api::stickers>(result);
    td::vector<int64> sticker_set_ids;
    for (const auto &sticker : stickers->stickers_) {
      sticker_set_ids.push_back(sticker->set_id_);
    }
    client_->get_sticker_set_names(
        std::move(sticker_set_ids),
        td::PromiseCreator::lambda([actor_id = client_->actor_id(client_), stickers = std::move(stickers),
                                    query = std::move(query_)](td::Unit) mutable {
          send_closure(actor_id, &Client::return_stickers, std::move(stickers), std::move(query));
        }));
  }

 private:
  Client *client_;
  PromisedQueryPtr query_;
};

class Client::TdOnSendCustomRequestCallback final : public TdQueryCallback {
 public:
  explicit TdOnSendCustomRequestCallback(PromisedQueryPtr query) : query_(std::move(query)) {
  }

  void on_result(object_ptr<td_api::Object> result) final {
    if (result->get_id() == td_api::error::ID) {
      return fail_query_with_error(std::move(query_), move_object_as<td_api::error>(result));
    }

    CHECK(result->get_id() == td_api::customRequestResult::ID);
    auto res = move_object_as<td_api::customRequestResult>(result);
    answer_query(JsonCustomJson(res->result_), std::move(query_));
  }

 private:
  PromisedQueryPtr query_;
};

void Client::close() {
  need_close_ = true;
  if (td_client_.empty()) {
    set_timeout_in(0);
  } else if (!closing_) {
    do_send_request(make_object<td_api::close>(), td::make_unique<TdOnOkCallback>());
  }
}

void Client::log_out(int32 error_code, td::Slice error_message) {
  LOG(WARNING) << "Logging out due to error " << error_code << ": " << error_message;
  if (error_message == "API_ID_INVALID") {
    is_api_id_invalid_ = true;
  } else if (error_code == 429) {
    auto retry_after_time = get_retry_after_time(error_message);
    if (retry_after_time > 0) {
      next_authorization_time_ = td::max(next_authorization_time_, td::Time::now() + retry_after_time);
    }
  } else if (error_code >= 500) {
    next_authorization_time_ = td::max(next_authorization_time_, td::Time::now() + 1);
  }
  if (!td_client_.empty() && !logging_out_ && !closing_) {
    do_send_request(make_object<td_api::logOut>(), td::make_unique<TdOnOkCallback>());
  }
}

std::size_t Client::get_pending_update_count() const {
  return parameters_->shared_data_->tqueue_->get_size(tqueue_id_);
}

void Client::update_last_synchronization_error_date() {
  if (disconnection_time_ == 0 || !was_authorized_ || logging_out_ || closing_) {
    return;
  }
  auto now = td::Time::now();
  if (last_update_creation_time_ > now - 10 || disconnection_time_ > now - 180) {
    return;
  }

  last_synchronization_error_date_ = get_unix_time();
}

ServerBotInfo Client::get_bot_info() const {
  ServerBotInfo res;
  res.id_ = bot_token_id_;
  res.token_ = bot_token_;
  auto user_info = get_user_info(my_id_);
  if (user_info != nullptr) {
    res.username_ = user_info->editable_username;
  } else if (!was_authorized_) {
    if (logging_out_) {
      res.username_ = "<failed to authorize>";
    } else {
      res.username_ = "<unauthorized>";
    }
  } else {
    res.username_ = "<unknown>";
  }
  res.webhook_ = webhook_url_;
  res.has_webhook_certificate_ = has_webhook_certificate_;
  auto &tqueue = parameters_->shared_data_->tqueue_;
  res.head_update_id_ = tqueue->get_head(tqueue_id_).value();
  res.tail_update_id_ = tqueue->get_tail(tqueue_id_).value();
  res.webhook_max_connections_ = webhook_max_connections_;
  res.pending_update_count_ = tqueue->get_size(tqueue_id_);
  res.start_time_ = start_time_;
  return res;
}

void Client::start_up() {
  CHECK(start_time_ < 1e-10);
  start_time_ = td::Time::now();
  next_bot_updates_warning_time_ = start_time_ + 600;
  webhook_set_time_ = start_time_;
  next_allowed_set_webhook_time_ = start_time_;
  next_set_webhook_logging_time_ = start_time_;
  next_webhook_is_not_modified_warning_time_ = start_time_;
  previous_get_updates_start_time_ = start_time_ - 100;
  next_get_updates_conflict_time_ = start_time_ - 100;

  sticker_set_names_[GREAT_MINDS_SET_ID] = GREAT_MINDS_SET_NAME.str();

  auto colon_pos = bot_token_.find_first_of(':');
  if (colon_pos == td::string::npos) {
    LOG(WARNING) << "Wrong bot token " << bot_token_;
    logging_out_ = true;
    return finish_closing();
  }
  bot_token_id_ = bot_token_.substr(0, colon_pos);

  auto base64_bot_token = bot_token_.substr(colon_pos + 1);
  if (td::base64url_decode(base64_bot_token).is_error() || base64_bot_token.size() < 24) {
    LOG(WARNING) << "Wrong bot token " << bot_token_;
    logging_out_ = true;
    return finish_closing();
  }

  bot_token_with_dc_ = bot_token_ + (is_test_dc_ ? ":T" : "");

  auto context = std::make_shared<td::ActorContext>();
  set_context(context);
  set_tag(bot_token_id_);

  auto suff = bot_token_with_dc_ + TD_DIR_SLASH;
  if (!parameters_->allow_colon_in_filenames_) {
    for (auto &c : suff) {
      if (c == ':') {
        c = '~';
      }
    }
  }
  dir_ = parameters_->working_directory_ + suff;
  files_dir_ = parameters_->files_directory_ + suff;

  class TdCallback final : public td::TdCallback {
   public:
    explicit TdCallback(td::ActorId<Client> client) : client_(std::move(client)) {
    }
    void on_result(td::uint64 id, object_ptr<td_api::Object> result) final {
      send_closure_later(client_, &Client::on_result, id, std::move(result));
    }
    void on_error(td::uint64 id, object_ptr<td_api::error> result) final {
      send_closure_later(client_, &Client::on_result, id, std::move(result));
    }

   private:
    td::ActorId<Client> client_;
  };
  td::ClientActor::Options options;
  options.net_query_stats = parameters_->net_query_stats_;
  td_client_ = td::create_actor_on_scheduler<td::ClientActor>(
      "TdClientActor", 0, td::make_unique<TdCallback>(actor_id(this)), std::move(options));
}

void Client::send(PromisedQueryPtr query) {
  if (!query->is_internal()) {
    query->set_stat_actor(stat_actor_);
    if (!parameters_->local_mode_ && !is_local_method(query->method()) &&
        td::Time::now() > parameters_->start_time_ + 60) {
      BotStatActor *stat = stat_actor_.get_actor_unsafe();
      auto update_per_minute = static_cast<int64>(stat->get_minute_update_count(td::Time::now()) * 60);
      if (stat->get_active_request_count() > 1000 + update_per_minute) {
        LOG(INFO) << "Fail a query, because there are too many active queries: " << *query;
        return fail_query_flood_limit_exceeded(std::move(query));
      }
      if (stat->get_active_file_upload_bytes() > (static_cast<int64>(1) << 32) && !query->files().empty()) {
        LOG(INFO) << "Fail a query, because the total size of active file uploads is too big: " << *query;
        return fail_query_flood_limit_exceeded(std::move(query));
      }
      if (stat->get_active_file_upload_count() > 100 + update_per_minute / 5 && !query->files().empty()) {
        LOG(INFO) << "Fail a query, because there are too many active file uploads: " << *query;
        return fail_query_flood_limit_exceeded(std::move(query));
      }
    }
  }
  cmd_queue_.emplace(std::move(query));
  loop();
}

void Client::raw_event(const td::Event::Raw &event) {
  long_poll_wakeup(true);
}

void Client::loop() {
  if (was_authorized_ || logging_out_ || closing_) {
    while (!cmd_queue_.empty()) {
      auto query = std::move(cmd_queue_.front());
      cmd_queue_.pop();
      on_cmd(std::move(query));
    }
  }
}

void Client::on_get_reply_message(int64 chat_id, object_ptr<td_api::message> reply_to_message) {
  auto &queue = new_message_queues_[chat_id];
  CHECK(!queue.queue_.empty());
  auto &message_info = queue.queue_.front().message_info_;
  CHECK(chat_id == message_info->chat_id);
  int64 reply_to_message_id = get_same_chat_reply_to_message_id(message_info.get(), false);
  CHECK(reply_to_message_id > 0);
  if (reply_to_message == nullptr) {
    LOG(INFO) << "Can't find message " << reply_to_message_id << " in chat " << chat_id
              << ". It is already deleted or inaccessible because of the chosen privacy mode";
  } else {
    if (chat_id != reply_to_message->chat_id_ || reply_to_message_id != reply_to_message->id_) {
      LOG(ERROR) << "Expect to get replied message " << reply_to_message_id << " in " << chat_id << ", but receive "
                 << reply_to_message->id_ << " in " << reply_to_message->chat_id_;
    }
    LOG(INFO) << "Receive reply to message " << reply_to_message->id_ << " in chat " << reply_to_message->chat_id_;
    add_message(std::move(reply_to_message));
  }

  process_new_message_queue(chat_id, 1);
}

void Client::on_get_edited_message(object_ptr<td_api::message> edited_message) {
  if (edited_message == nullptr) {
    LOG(INFO) << "Can't find just edited message. It is already deleted or inaccessible because of chosen privacy mode";
  } else {
    add_new_message(std::move(edited_message), true);
  }
}

void Client::on_get_callback_query_message(object_ptr<td_api::message> message, int64 user_id, int state) {
  CHECK(user_id != 0);
  auto &queue = new_callback_query_queues_[user_id];
  CHECK(!queue.queue_.empty());
  int64 chat_id = queue.queue_.front()->chat_id_;
  int64 message_id = queue.queue_.front()->message_id_;
  if (message == nullptr) {
    if (state == 0) {
      LOG(INFO) << "Can't find callback query message " << message_id << " in chat " << chat_id
                << ". It may be already deleted";
    } else {
      CHECK(state == 1);
      auto message_info = get_message(chat_id, message_id, true);
      if (message_info == nullptr) {
        LOG(INFO) << "Can't find callback query message " << message_id << " in chat " << chat_id
                  << ". It may be already deleted, while searcing for its reply to message";
        process_new_callback_query_queue(user_id, state);
        return;
      }
      auto reply_to_message_id = get_same_chat_reply_to_message_id(message_info, false);
      LOG(INFO) << "Can't find callback query reply to message " << reply_to_message_id << " in chat " << chat_id
                << ". It may be already deleted";
    }
  } else {
    LOG(INFO) << "Receive callback query " << (state == 1 ? "reply to " : "") << "message " << message_id << " in chat "
              << chat_id;
    add_message(std::move(message));
  }
  process_new_callback_query_queue(user_id, state + 1);
}

void Client::get_sticker_set_names(td::vector<int64> sticker_set_ids, td::Promise<td::Unit> &&promise) {
  td::FlatHashSet<int64> missing_sticker_set_ids;
  for (auto sticker_set_id : sticker_set_ids) {
    if (sticker_set_id != 0 && sticker_set_names_.count(sticker_set_id) == 0) {
      missing_sticker_set_ids.insert(sticker_set_id);
    }
  }
  if (missing_sticker_set_ids.empty()) {
    // fast pass
    return promise.set_value(td::Unit());
  }

  td::MultiPromiseActorSafe mpas("GetStickerSetNamesMultiPromiseActor");
  mpas.add_promise(std::move(promise));
  mpas.set_ignore_errors(true);

  auto lock = mpas.get_promise();
  for (auto sticker_set_id : missing_sticker_set_ids) {
    send_request(make_object<td_api::getStickerSetName>(sticker_set_id),
                 td::make_unique<TdOnGetStickerSetNameCallback>(this, sticker_set_id, mpas.get_promise()));
  }
  lock.set_value(td::Unit());
}

void Client::on_get_sticker_set_name(int64 set_id, const td::string &name) {
  CHECK(set_id != 0);
  if (set_id != GREAT_MINDS_SET_ID) {
    sticker_set_names_[set_id] = name;
  }
}

template <class OnSuccess>
void Client::check_user_read_access(const UserInfo *user_info, PromisedQueryPtr query, OnSuccess on_success) {
  CHECK(user_info != nullptr);
  if (!user_info->have_access) {
    // return fail_query(400, "Bad Request: have no access to the user", std::move(query));
  }
  on_success(std::move(query));
}

template <class OnSuccess>
void Client::check_user(int64 user_id, PromisedQueryPtr query, OnSuccess on_success) {
  const UserInfo *user_info = get_user_info(user_id);
  if (user_info != nullptr && user_info->have_access) {
    return check_user_read_access(user_info, std::move(query), std::move(on_success));
  }
  send_request(make_object<td_api::getUser>(user_id),
               td::make_unique<TdOnCheckUserCallback<OnSuccess>>(this, std::move(query), std::move(on_success)));
}

template <class OnSuccess>
void Client::check_user_no_fail(int64 user_id, PromisedQueryPtr query, OnSuccess on_success) {
  const UserInfo *user_info = get_user_info(user_id);
  if (user_info != nullptr && user_info->have_access) {
    on_success(std::move(query));
    return;
  }
  send_request(make_object<td_api::getUser>(user_id),
               td::make_unique<TdOnCheckUserNoFailCallback<OnSuccess>>(std::move(query), std::move(on_success)));
}

template <class OnSuccess>
void Client::check_chat_access(int64 chat_id, AccessRights access_rights, const ChatInfo *chat_info,
                               PromisedQueryPtr query, OnSuccess on_success) const {
  CHECK(chat_info != nullptr);
  bool need_write_access = access_rights == AccessRights::Write;
  bool need_edit_access = access_rights == AccessRights::Edit || need_write_access;
  bool need_read_access = true;
  switch (chat_info->type) {
    case ChatInfo::Type::Private: {
      auto user_info = get_user_info(chat_info->user_id);
      CHECK(user_info != nullptr);
      if (user_info->type == UserInfo::Type::Deleted && need_edit_access) {
        return fail_query(403, "Forbidden: user is deactivated", std::move(query));
      }
      if (user_info->type == UserInfo::Type::Unknown) {
        return fail_query(400, "Bad Request: private chat not found", std::move(query));
      }
      break;
    }
    case ChatInfo::Type::Group: {
      if (access_rights == AccessRights::ReadMembers) {  // member list is inaccessible in deactivated groups
        need_write_access = true;
        need_edit_access = true;
      }
      auto group_info = get_group_info(chat_info->group_id);
      CHECK(group_info != nullptr);
      if (!group_info->is_active && need_write_access) {
        if (group_info->upgraded_to_supergroup_id != 0) {
          td::FlatHashMap<td::string, td::unique_ptr<td::VirtuallyJsonable>> parameters;
          auto updagraded_to_chat_id = get_supergroup_chat_id(group_info->upgraded_to_supergroup_id);
          parameters.emplace("migrate_to_chat_id", td::make_unique<td::VirtuallyJsonableLong>(updagraded_to_chat_id));
          return fail_query(400, "Bad Request: group chat was upgraded to a supergroup chat", std::move(query),
                            std::move(parameters));
        } else {
          return fail_query(403, "Forbidden: the group chat was deleted", std::move(query));
        }
      }
      if (group_info->is_active && group_info->kicked && need_edit_access) {
        return fail_query(403, "Forbidden: bot was kicked from the group chat", std::move(query));
      }
      if (group_info->is_active && group_info->left && need_edit_access) {
        return fail_query(403, "Forbidden: bot is not a member of the group chat", std::move(query));
      }
      break;
    }
    case ChatInfo::Type::Supergroup: {
      auto supergroup_info = get_supergroup_info(chat_info->supergroup_id);
      CHECK(supergroup_info != nullptr);
      if (supergroup_info->status->get_id() == td_api::chatMemberStatusBanned::ID) {
        if (supergroup_info->is_supergroup) {
          return fail_query(403, "Forbidden: bot was kicked from the supergroup chat", std::move(query));
        } else {
          return fail_query(403, "Forbidden: bot was kicked from the channel chat", std::move(query));
        }
      }
      bool is_public = !supergroup_info->active_usernames.empty() || supergroup_info->has_location;
      bool need_more_access_rights = is_public ? need_edit_access : need_read_access;
      if (!is_chat_member(supergroup_info->status) && need_more_access_rights) {
        if (supergroup_info->is_supergroup) {
          return fail_query(403, "Forbidden: bot is not a member of the supergroup chat", std::move(query));
        } else {
          return fail_query(403, "Forbidden: bot is not a member of the channel chat", std::move(query));
        }
      }
      break;
    }
    case ChatInfo::Type::Unknown:
    default:
      UNREACHABLE();
  }
  on_success(chat_id, std::move(query));
}

template <class OnSuccess>
void Client::check_chat(td::Slice chat_id_str, AccessRights access_rights, PromisedQueryPtr query, OnSuccess on_success,
                        bool allow_unknown_user) {
  if (chat_id_str.empty()) {
    return fail_query(400, "Bad Request: chat_id is empty", std::move(query));
  }

  if (chat_id_str[0] == '@') {
    return send_request(make_object<td_api::searchPublicChat>(chat_id_str.str()),
                        td::make_unique<TdOnCheckChatCallback<OnSuccess>>(this, true, access_rights, std::move(query),
                                                                          std::move(on_success)));
  }

  auto chat_id = td::to_integer<int64>(chat_id_str);
  if (allow_unknown_user && 0 < chat_id && chat_id < (static_cast<int64>(1) << 40)) {
    return on_success(chat_id, std::move(query));
  }
  auto chat_info = get_chat(chat_id);
  if (chat_info != nullptr && chat_info->type == ChatInfo::Type::Private) {
    const UserInfo *user_info = get_user_info(chat_info->user_id);
    if (user_info == nullptr || !user_info->have_access) {
      chat_info = nullptr;
    }
  }
  if (chat_info != nullptr) {
    return check_chat_access(chat_id, access_rights, chat_info, std::move(query), std::move(on_success));
  }
  send_request(make_object<td_api::getChat>(chat_id),
               td::make_unique<TdOnCheckChatCallback<OnSuccess>>(this, false, access_rights, std::move(query),
                                                                 std::move(on_success)));
}

template <class OnSuccess>
void Client::check_chat_no_fail(td::Slice chat_id_str, PromisedQueryPtr query, OnSuccess on_success) {
  if (chat_id_str.empty()) {
    return fail_query(400, "Bad Request: sender_chat_id is empty", std::move(query));
  }

  auto r_chat_id = td::to_integer_safe<int64>(chat_id_str);
  if (r_chat_id.is_error()) {
    return fail_query(400, "Bad Request: sender_chat_id is not a valid Integer", std::move(query));
  }
  auto chat_id = r_chat_id.move_as_ok();

  auto chat_info = get_chat(chat_id);
  if (chat_info != nullptr && chat_info->type == ChatInfo::Type::Private) {
    const UserInfo *user_info = get_user_info(chat_info->user_id);
    if (user_info == nullptr || !user_info->have_access) {
      chat_info = nullptr;
    }
  }
  if (chat_info != nullptr) {
    return on_success(chat_id, std::move(query));
  }
  send_request(make_object<td_api::getChat>(chat_id), td::make_unique<TdOnCheckChatNoFailCallback<OnSuccess>>(
                                                          chat_id, std::move(query), std::move(on_success)));
}

template <class OnSuccess>
void Client::check_message_topic(td::Slice chat_id_str, int32 forum_topic_id, int64 direct_messages_topic_id,
                                 PromisedQueryPtr query, OnSuccess on_success, bool allow_unknown_user) {
  check_chat(
      chat_id_str, AccessRights::Write, std::move(query),
      [this, forum_topic_id, direct_messages_topic_id, allow_unknown_user, on_success = std::move(on_success)](
          int64 chat_id, PromisedQueryPtr query) mutable {
        if (forum_topic_id == 0 && direct_messages_topic_id == 0) {
          return on_success(chat_id, nullptr, std::move(query));
        }
        auto chat_info = get_chat(chat_id);
        CHECK(chat_info != nullptr || allow_unknown_user);
        bool can_be_forum = false;
        if (chat_info != nullptr && chat_info->type == ChatInfo::Type::Supergroup) {
          auto supergroup_info = get_supergroup_info(chat_info->supergroup_id);
          CHECK(supergroup_info != nullptr);
          if (supergroup_info->is_direct_messages) {
            return on_success(chat_id, make_object<td_api::messageTopicDirectMessages>(direct_messages_topic_id),
                              std::move(query));
          }
          if (supergroup_info->is_forum) {
            if (forum_topic_id == GENERAL_FORUM_TOPIC_ID) {
              return fail_query(400, "Bad Request: message thread not found", std::move(query));
            }
            can_be_forum = true;
          }
        } else if (chat_info != nullptr && chat_info->type == ChatInfo::Type::Private) {
          can_be_forum = true;
        }
        if (can_be_forum && forum_topic_id != 0) {
          return send_request(make_object<td_api::getForumTopic>(chat_id, forum_topic_id),
                              td::make_unique<TdOnCheckForumTopicCallback<OnSuccess>>(
                                  this, chat_id, forum_topic_id, std::move(query), std::move(on_success)));
        }
        return on_success(chat_id, nullptr, std::move(query));
      },
      allow_unknown_user && forum_topic_id == 0);
}

td::Result<td::int64> Client::get_business_connection_chat_id(td::Slice chat_id_str) {
  if (chat_id_str.empty()) {
    return td::Status::Error(400, "Bad Request: chat identifier is empty");
  }

  auto r_chat_id = td::to_integer_safe<int64>(chat_id_str);
  if (r_chat_id.is_error()) {
    return td::Status::Error(400, "Bad Request: chat identifier must be a valid Integer");
  }
  return r_chat_id.move_as_ok();
}

template <class OnSuccess>
void Client::check_business_connection(const td::string &business_connection_id, PromisedQueryPtr query,
                                       OnSuccess on_success) {
  auto business_connection = get_business_connection(business_connection_id);
  if (business_connection != nullptr) {
    return on_success(business_connection, std::move(query));
  }
  send_request(
      make_object<td_api::getBusinessConnection>(business_connection_id),
      td::make_unique<TdOnCheckBusinessConnectionCallback<OnSuccess>>(this, std::move(query), std::move(on_success)));
}

template <class OnSuccess>
void Client::check_business_connection_chat_id(const td::string &business_connection_id, const td::string &chat_id_str,
                                               PromisedQueryPtr query, OnSuccess on_success) {
  auto r_chat_id = get_business_connection_chat_id(chat_id_str);
  if (r_chat_id.is_error()) {
    return fail_query_with_error(std::move(query), 400, r_chat_id.error().message());
  }
  auto chat_id = r_chat_id.move_as_ok();
  check_business_connection(business_connection_id, std::move(query),
                            [chat_id, on_success = std::move(on_success)](const BusinessConnection *business_connection,
                                                                          PromisedQueryPtr query) mutable {
                              on_success(business_connection, chat_id, std::move(query));
                            });
}

template <class OnSuccess>
void Client::check_bot_command_scope(BotCommandScope &&scope, PromisedQueryPtr query, OnSuccess on_success) {
  CHECK(scope.scope_ != nullptr);
  if (scope.chat_id_.empty()) {
    on_success(std::move(scope.scope_), std::move(query));
    return;
  }
  check_chat(scope.chat_id_, AccessRights::ReadMembers, std::move(query),
             [this, user_id = scope.user_id_, scope_id = scope.scope_->get_id(), on_success = std::move(on_success)](
                 int64 chat_id, PromisedQueryPtr query) mutable {
               switch (scope_id) {
                 case td_api::botCommandScopeChat::ID:
                   on_success(make_object<td_api::botCommandScopeChat>(chat_id), std::move(query));
                   break;
                 case td_api::botCommandScopeChatAdministrators::ID:
                   on_success(make_object<td_api::botCommandScopeChatAdministrators>(chat_id), std::move(query));
                   break;
                 case td_api::botCommandScopeChatMember::ID:
                   check_user_no_fail(
                       user_id, std::move(query),
                       [chat_id, user_id, on_success = std::move(on_success)](PromisedQueryPtr query) mutable {
                         on_success(make_object<td_api::botCommandScopeChatMember>(chat_id, user_id), std::move(query));
                       });
                   break;
                 default:
                   UNREACHABLE();
               }
             });
}

template <class OnSuccess>
void Client::check_remote_file_id(td::string file_id, PromisedQueryPtr query, OnSuccess on_success) {
  if (file_id.empty()) {
    return fail_query(400, "Bad Request: file_id not specified", std::move(query));
  }

  send_request(make_object<td_api::getRemoteFile>(std::move(file_id), nullptr),
               td::make_unique<TdOnCheckRemoteFileIdCallback<OnSuccess>>(std::move(query), std::move(on_success)));
}

bool Client::is_chat_member(const object_ptr<td_api::ChatMemberStatus> &status) {
  switch (status->get_id()) {
    case td_api::chatMemberStatusBanned::ID:
    case td_api::chatMemberStatusLeft::ID:
      return false;
    case td_api::chatMemberStatusRestricted::ID:
      return static_cast<const td_api::chatMemberStatusRestricted *>(status.get())->is_member_;
    default:
      // ignore Creator.is_member_
      return true;
  }
}

bool Client::have_message_access(int64 chat_id) const {
  if (chat_id > 0) {
    return true;  // for unknown private chats during paid broadcasts
  }
  auto chat_info = get_chat(chat_id);
  CHECK(chat_info != nullptr);
  switch (chat_info->type) {
    case ChatInfo::Type::Private:
    case ChatInfo::Type::Group:
      return true;
    case ChatInfo::Type::Supergroup: {
      auto supergroup_info = get_supergroup_info(chat_info->supergroup_id);
      CHECK(supergroup_info != nullptr);
      if (!is_chat_member(supergroup_info->status)) {
        LOG(DEBUG) << "Have no access to messages of " << chat_id;
        return false;
      }
      return true;
    }
    case ChatInfo::Type::Unknown:
    default:
      UNREACHABLE();
      return false;
  }
}

template <class OnSuccess>
void Client::check_message(td::Slice chat_id_str, int64 message_id, bool allow_empty, AccessRights access_rights,
                           td::Slice message_type, PromisedQueryPtr query, OnSuccess on_success) {
  check_chat(chat_id_str, access_rights, std::move(query),
             [this, message_id, allow_empty, message_type, on_success = std::move(on_success)](
                 int64 chat_id, PromisedQueryPtr query) mutable {
               if ((message_id <= 0 && !allow_empty) || !have_message_access(chat_id)) {
                 return fail_query_with_error(std::move(query), 400, "MESSAGE_NOT_FOUND",
                                              PSLICE() << message_type << " not found");
               }

               if (message_id <= 0) {
                 CHECK(allow_empty);
                 return on_success(chat_id, 0, std::move(query));
               }

               send_request(
                   make_object<td_api::getMessage>(chat_id, message_id),
                   td::make_unique<TdOnCheckMessageCallback<OnSuccess>>(
                       this, chat_id, message_id, allow_empty, message_type, std::move(query), std::move(on_success)));
             });
}

template <class OnSuccess>
void Client::check_messages(td::Slice chat_id_str, td::vector<int64> message_ids, bool allow_empty,
                            AccessRights access_rights, td::Slice message_type, PromisedQueryPtr query,
                            OnSuccess on_success) {
  check_chat(chat_id_str, access_rights, std::move(query),
             [this, message_ids = std::move(message_ids), allow_empty, message_type,
              on_success = std::move(on_success)](int64 chat_id, PromisedQueryPtr query) mutable {
               if (!have_message_access(chat_id)) {
                 return fail_query_with_error(std::move(query), 400, "MESSAGE_NOT_FOUND",
                                              PSLICE() << message_type << " not found");
               }

               send_request(make_object<td_api::getMessages>(chat_id, std::move(message_ids)),
                            td::make_unique<TdOnCheckMessagesCallback<OnSuccess>>(
                                this, chat_id, allow_empty, message_type, std::move(query), std::move(on_success)));
             });
}

bool Client::is_reply_in_same_topic(const object_ptr<td_api::MessageTopic> &reply_topic_id,
                                    const object_ptr<td_api::MessageTopic> &topic_id) {
  if (topic_id == nullptr) {
    // if topic isn't specified explicitly, then it is definitely the same
    return true;
  }
  switch (topic_id->get_id()) {
    case td_api::messageTopicForum::ID: {
      auto forum_topic_id = static_cast<const td_api::messageTopicForum *>(topic_id.get())->forum_topic_id_;
      return get_forum_topic_id(reply_topic_id) == forum_topic_id;
    }
    case td_api::messageTopicDirectMessages::ID:
      // direct messages can't be answered in a different topic
      return true;
    default:
      UNREACHABLE();
  }
}

template <class OnSuccess>
void Client::check_reply_parameters(td::Slice chat_id_str, InputReplyParameters &&reply_parameters,
                                    int32 forum_topic_id, int64 direct_messages_topic_id, PromisedQueryPtr query,
                                    OnSuccess on_success, bool allow_unknown_user) {
  if (chat_id_str == reply_parameters.reply_in_chat_id) {
    reply_parameters.reply_in_chat_id.clear();
  }
  check_message_topic(
      chat_id_str, forum_topic_id, direct_messages_topic_id, std::move(query),
      [this, reply_parameters = std::move(reply_parameters), on_success = std::move(on_success)](
          int64 chat_id, object_ptr<td_api::MessageTopic> &&topic_id, PromisedQueryPtr query) mutable {
        if (reply_parameters.reply_to_ephemeral_message_id > 0) {
          CheckedReplyParameters checked_reply_parameters;
          checked_reply_parameters.reply_to_ephemeral_message_id = reply_parameters.reply_to_ephemeral_message_id;
          return on_success(chat_id, std::move(topic_id), std::move(checked_reply_parameters), std::move(query));
        }
        auto on_reply_message_resolved =
            [this, chat_id, topic_id = std::move(topic_id), quote = std::move(reply_parameters.quote),
             checklist_task_id = reply_parameters.checklist_task_id,
             poll_option_id = std::move(reply_parameters.poll_option_id), on_success = std::move(on_success)](
                int64 reply_in_chat_id, int64 reply_to_message_id, PromisedQueryPtr query) mutable {
              CheckedReplyParameters reply_parameters;
              reply_parameters.reply_to_message_id = reply_to_message_id;
              if (reply_to_message_id > 0) {
                reply_parameters.reply_in_chat_id = reply_in_chat_id;
                reply_parameters.quote = std::move(quote);
                reply_parameters.checklist_task_id = checklist_task_id;
                reply_parameters.poll_option_id = std::move(poll_option_id);
              }

              // reply_in_chat_id must be non-zero only for replies in different chats or different topics
              if (reply_to_message_id > 0 && reply_in_chat_id == chat_id) {
                const MessageInfo *message_info = get_message(reply_in_chat_id, reply_to_message_id, true);
                CHECK(message_info != nullptr);
                if (is_reply_in_same_topic(message_info->topic_id, topic_id)) {
                  reply_parameters.reply_in_chat_id = 0;
                }
              }
              return on_success(chat_id, std::move(topic_id), std::move(reply_parameters), std::move(query));
            };
        if (reply_parameters.reply_to_message_id <= 0) {
          return on_reply_message_resolved(0, 0, std::move(query));
        }

        auto on_reply_chat_resolved = [this, reply_to_message_id = reply_parameters.reply_to_message_id,
                                       allow_sending_without_reply = reply_parameters.allow_sending_without_reply,
                                       on_success = std::move(on_reply_message_resolved)](
                                          int64 reply_in_chat_id, PromisedQueryPtr query) mutable {
          if (!have_message_access(reply_in_chat_id)) {
            return fail_query_with_error(std::move(query), 400, "MESSAGE_NOT_FOUND", "message to be replied not found");
          }

          send_request(make_object<td_api::getMessage>(reply_in_chat_id, reply_to_message_id),
                       td::make_unique<TdOnCheckMessageCallback<decltype(on_success)>>(
                           this, reply_in_chat_id, reply_to_message_id, allow_sending_without_reply,
                           "message to be replied", std::move(query), std::move(on_success)));
        };
        if (reply_parameters.reply_in_chat_id.empty()) {
          return on_reply_chat_resolved(chat_id, std::move(query));
        }
        check_chat(reply_parameters.reply_in_chat_id, AccessRights::Read, std::move(query),
                   std::move(on_reply_chat_resolved));
      },
      allow_unknown_user);
}

template <class OnSuccess>
void Client::resolve_sticker_set(const td::string &sticker_set_name, PromisedQueryPtr query, OnSuccess on_success) {
  if (sticker_set_name.empty()) {
    return fail_query(400, "Bad Request: sticker_set_name is empty", std::move(query));
  }

  send_request(make_object<td_api::searchStickerSet>(sticker_set_name, false),
               td::make_unique<TdOnSearchStickerSetCallback<OnSuccess>>(std::move(query), std::move(on_success)));
}

void Client::fix_reply_markup_bot_user_ids(object_ptr<td_api::ReplyMarkup> &reply_markup) const {
  if (reply_markup == nullptr || reply_markup->get_id() != td_api::replyMarkupInlineKeyboard::ID) {
    return;
  }
  auto inline_keyboard = static_cast<td_api::replyMarkupInlineKeyboard *>(reply_markup.get());
  for (auto &row : inline_keyboard->rows_) {
    for (auto &button : row) {
      CHECK(button != nullptr);
      CHECK(button->type_ != nullptr);
      if (button->type_->get_id() != td_api::inlineKeyboardButtonTypeLoginUrl::ID) {
        continue;
      }
      auto login_url_button = static_cast<td_api::inlineKeyboardButtonTypeLoginUrl *>(button->type_.get());
      if (login_url_button->id_ % 1000 != 0 || login_url_button->id_ == 0) {
        continue;
      }
      auto it = temp_to_real_bot_user_id_.find(std::abs(login_url_button->id_));
      CHECK(it != temp_to_real_bot_user_id_.end());
      auto bot_user_id = it->second;
      CHECK(bot_user_id != 0);
      if (login_url_button->id_ < 0) {
        login_url_button->id_ = -bot_user_id - 1;
      } else {
        login_url_button->id_ = bot_user_id;
      }
    }
  }
}

void Client::fix_inline_query_results_bot_user_ids(
    td::vector<object_ptr<td_api::InputInlineQueryResult>> &results) const {
  for (auto &result : results) {
    td_api::downcast_call(
        *result, [this](auto &result_type) { this->fix_reply_markup_bot_user_ids(result_type.reply_markup_); });
  }
}

void Client::resolve_bot_usernames(PromisedQueryPtr query, td::Promise<PromisedQueryPtr> on_success) {
  CHECK(!bot_user_ids_.unresolved_bot_usernames_.empty());
  auto query_id = current_bot_resolve_query_id_++;
  auto &pending_query = pending_bot_resolve_queries_[query_id];
  pending_query.pending_resolve_count = bot_user_ids_.unresolved_bot_usernames_.size();
  pending_query.query = std::move(query);
  pending_query.on_success = std::move(on_success);
  for (auto &username : bot_user_ids_.unresolved_bot_usernames_) {
    auto &query_ids = awaiting_bot_resolve_queries_[username];
    query_ids.push_back(query_id);
    if (query_ids.size() == 1) {
      send_request(make_object<td_api::searchPublicChat>(username),
                   td::make_unique<TdOnResolveBotUsernameCallback>(this, username));
    }
  }
  bot_user_ids_.unresolved_bot_usernames_.clear();
}

template <class OnSuccess>
void Client::resolve_reply_markup_bot_usernames(object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query,
                                                OnSuccess on_success) {
  if (!bot_user_ids_.unresolved_bot_usernames_.empty()) {
    CHECK(reply_markup != nullptr);
    CHECK(reply_markup->get_id() == td_api::replyMarkupInlineKeyboard::ID);
    return resolve_bot_usernames(
        std::move(query),
        td::PromiseCreator::lambda([this, reply_markup = std::move(reply_markup),
                                    on_success = std::move(on_success)](td::Result<PromisedQueryPtr> result) mutable {
          if (result.is_ok()) {
            fix_reply_markup_bot_user_ids(reply_markup);
            on_success(std::move(reply_markup), result.move_as_ok());
          }
        }));
  }
  on_success(std::move(reply_markup), std::move(query));
}

template <class OnSuccess>
void Client::resolve_inline_query_results_bot_usernames(td::vector<object_ptr<td_api::InputInlineQueryResult>> results,
                                                        PromisedQueryPtr query, OnSuccess on_success) {
  if (!bot_user_ids_.unresolved_bot_usernames_.empty()) {
    return resolve_bot_usernames(
        std::move(query),
        td::PromiseCreator::lambda([this, results = std::move(results),
                                    on_success = std::move(on_success)](td::Result<PromisedQueryPtr> result) mutable {
          if (result.is_ok()) {
            fix_inline_query_results_bot_user_ids(results);
            on_success(std::move(results), result.move_as_ok());
          }
        }));
  }
  on_success(std::move(results), std::move(query));
}

void Client::on_resolve_bot_username(const td::string &username, int64 user_id) {
  auto query_ids_it = awaiting_bot_resolve_queries_.find(username);
  CHECK(query_ids_it != awaiting_bot_resolve_queries_.end());
  CHECK(!query_ids_it->second.empty());
  auto query_ids = std::move(query_ids_it->second);
  awaiting_bot_resolve_queries_.erase(query_ids_it);

  if (user_id == 0) {
    bot_user_ids_.bot_user_ids_.erase(username);
  } else {
    auto &temp_bot_user_id = bot_user_ids_.bot_user_ids_[username];
    temp_to_real_bot_user_id_[temp_bot_user_id] = user_id;
    temp_bot_user_id = user_id;
  }

  for (auto query_id : query_ids) {
    auto it = pending_bot_resolve_queries_.find(query_id);
    if (it == pending_bot_resolve_queries_.end()) {
      // the query has already failed
      continue;
    }
    CHECK(it->second.pending_resolve_count > 0);
    it->second.pending_resolve_count--;
    if (it->second.pending_resolve_count == 0 || user_id == 0) {
      if (user_id == 0) {
        fail_query(400, PSTRING() << "Bad Request: bot \"" << username << "\" not found", std::move(it->second.query));
      } else {
        it->second.on_success.set_value(std::move(it->second.query));
      }
      pending_bot_resolve_queries_.erase(it);
    }
  }
}

template <class OnSuccess>
void Client::get_chat_member(int64 chat_id, int64 user_id, PromisedQueryPtr query, OnSuccess on_success) {
  check_user_no_fail(
      user_id, std::move(query),
      [this, chat_id, user_id, on_success = std::move(on_success)](PromisedQueryPtr query) mutable {
        send_request(make_object<td_api::getChatMember>(chat_id, make_object<td_api::messageSenderUser>(user_id)),
                     td::make_unique<TdOnGetChatMemberCallback<OnSuccess>>(std::move(query), std::move(on_success)));
      });
}

void Client::send_request(object_ptr<td_api::Function> &&f, td::unique_ptr<TdQueryCallback> handler) {
  if (closing_ || logging_out_) {
    auto error = get_closing_error();
    return handler->on_result(make_object<td_api::error>(error.code, error.message.str()));
  }

  do_send_request(std::move(f), std::move(handler));
}

void Client::do_send_request(object_ptr<td_api::Function> &&f, td::unique_ptr<TdQueryCallback> handler) {
  CHECK(!td_client_.empty());
  auto id = handlers_.create(std::move(handler));
  send_closure(td_client_, &td::ClientActor::request, id, std::move(f));
}

td_api::object_ptr<td_api::Object> Client::execute(object_ptr<td_api::Function> &&f) {
  return td::ClientActor::execute(std::move(f));
}

void Client::on_update_file(object_ptr<td_api::file> file) {
  auto file_id = file->id_;
  if (!is_file_being_downloaded(file_id)) {
    return;
  }
  if (!parameters_->local_mode_ && file->local_->downloaded_size_ > MAX_DOWNLOAD_FILE_SIZE) {
    if (file->local_->is_downloading_active_) {
      send_request(make_object<td_api::cancelDownloadFile>(file_id, false),
                   td::make_unique<TdOnCancelDownloadFileCallback>());
    }
    return on_file_download(file_id, td::Status::Error(400, "Bad Request: file is too big"));
  }
  if (file->local_->is_downloading_completed_) {
    return on_file_download(file_id, std::move(file));
  }
  if (!file->local_->is_downloading_active_ && download_started_file_ids_.count(file_id)) {
    // also includes all 5xx and 429 errors
    if (closing_ || logging_out_) {
      auto error = get_closing_error();
      return on_file_download(file_id, td::Status::Error(error.code, error.message));
    }

    auto error = td::Status::Error(400, "Bad Request: wrong file_id or the file is temporarily unavailable");
    return on_file_download(file_id, std::move(error));
  }
}

void Client::on_update_authorization_state() {
  CHECK(authorization_state_ != nullptr);
  switch (authorization_state_->get_id()) {
    case td_api::authorizationStateWaitTdlibParameters::ID: {
      for (td::string option : {"disable_network_statistics", "disable_time_adjustment_protection", "ignore_file_names",
                                "ignore_inline_thumbnails", "reuse_uploaded_photos_by_hash",
                                "store_all_files_in_files_directory", "use_storage_optimizer"}) {
        send_request(make_object<td_api::setOption>(option, make_object<td_api::optionValueBoolean>(true)),
                     td::make_unique<TdOnOkCallback>());
      }
      if (parameters_->storage_max_time_from_last_access_ >= 0) {
        send_request(make_object<td_api::setOption>(
                         "storage_max_time_from_last_access",
                         make_object<td_api::optionValueInteger>(parameters_->storage_max_time_from_last_access_)),
                     td::make_unique<TdOnOkCallback>());
      }

      auto request = make_object<td_api::setTdlibParameters>();
      request->use_test_dc_ = is_test_dc_;
      request->database_directory_ = dir_;
      request->files_directory_ = files_dir_;
      //request->use_file_database_ = false;
      //request->use_chat_info_database_ = false;
      //request->use_secret_chats_ = false;
      request->use_message_database_ = USE_MESSAGE_DATABASE;
      request->api_id_ = parameters_->api_id_;
      request->api_hash_ = parameters_->api_hash_;
      request->system_language_code_ = "en";
      request->device_model_ = "server";
      request->application_version_ = parameters_->version_;

      return send_request(std::move(request), td::make_unique<TdOnInitCallback>(this));
    }
    case td_api::authorizationStateWaitPhoneNumber::ID:
      send_request(make_object<td_api::setOption>("online", make_object<td_api::optionValueBoolean>(true)),
                   td::make_unique<TdOnOkCallback>());
      return send_request(make_object<td_api::checkAuthenticationBotToken>(bot_token_),
                          td::make_unique<TdOnAuthorizationCallback>(this));
    case td_api::authorizationStateReady::ID: {
      auto user_info = get_user_info(my_id_);
      if (my_id_ <= 0 || user_info == nullptr) {
        LOG(INFO) << "Send getMe request for " << my_id_;
        return send_request(make_object<td_api::getMe>(), td::make_unique<TdOnAuthorizationCallback>(this));
      }

      if (!was_authorized_) {
        LOG(WARNING) << "Logged in as @" << user_info->editable_username;
        was_authorized_ = true;
        td::send_event(parent_, td::Event::raw(static_cast<void *>(this)));
        update_shared_unix_time_difference();
        if (!pending_updates_.empty()) {
          LOG(INFO) << "Process " << pending_updates_.size() << " pending updates";
          for (auto &update : pending_updates_) {
            on_update(std::move(update));
          }
          td::reset_to_empty(pending_updates_);
        }
        last_update_creation_time_ = td::Time::now();
        log_in_date_ = get_unix_time();
      }
      return loop();
    }
    case td_api::authorizationStateLoggingOut::ID:
      if (!logging_out_) {
        LOG(WARNING) << "Logging out";
        update_last_synchronization_error_date();
        logging_out_ = true;
        if (was_authorized_ && !closing_) {
          td::send_event(parent_, td::Event::raw(nullptr));
        }
      }
      return loop();
    case td_api::authorizationStateClosing::ID:
      if (!closing_) {
        LOG(WARNING) << "Closing";
        update_last_synchronization_error_date();
        closing_ = true;
        if (was_authorized_ && !logging_out_) {
          td::send_event(parent_, td::Event::raw(nullptr));
        }
      }
      return loop();
    case td_api::authorizationStateClosed::ID:
      return on_closed();
    default:
      return log_out(500, "Unknown authorization state");  // just in case
  }
}

bool Client::allow_update_before_authorization(const td_api::Object *update) const {
  auto update_id = update->get_id();
  if (update_id == td_api::updateAuthorizationState::ID) {
    return true;
  }
  if (update_id == td_api::updateOption::ID) {
    const auto &name = static_cast<const td_api::updateOption *>(update)->name_;
    return name == "my_id" || name == "unix_time";
  }
  if (update_id == td_api::updateUser::ID) {
    return true;
  }
  return false;
}

void Client::update_shared_unix_time_difference() {
  CHECK(was_authorized_);
  LOG_IF(ERROR, local_unix_time_difference_ == 0) << "Unix time difference was not updated";
  auto data = parameters_->shared_data_.get();
  if (local_unix_time_difference_ > data->unix_time_difference_) {
    data->unix_time_difference_ = local_unix_time_difference_;
  }
}

void Client::on_update(object_ptr<td_api::Object> result) {
  if (!was_authorized_ && !allow_update_before_authorization(result.get())) {
    pending_updates_.push_back(std::move(result));
    return;
  }
  switch (result->get_id()) {
    case td_api::updateAuthorizationState::ID: {
      auto update = move_object_as<td_api::updateAuthorizationState>(result);
      authorization_state_ = std::move(update->authorization_state_);
      on_update_authorization_state();
      break;
    }
    case td_api::updateNewMessage::ID: {
      auto update = move_object_as<td_api::updateNewMessage>(result);
      add_new_message(std::move(update->message_), false);
      break;
    }
    case td_api::updateMessageSendSucceeded::ID: {
      auto update = move_object_as<td_api::updateMessageSendSucceeded>(result);
      on_message_send_succeeded(std::move(update->message_), update->old_message_id_);
      break;
    }
    case td_api::updateMessageSendFailed::ID: {
      auto update = move_object_as<td_api::updateMessageSendFailed>(result);
      on_message_send_failed(update->message_->chat_id_, update->old_message_id_, update->message_->id_,
                             std::move(update->error_));
      break;
    }
    case td_api::updateStoryPostSucceeded::ID: {
      auto update = move_object_as<td_api::updateStoryPostSucceeded>(result);
      on_story_send_succeeded(std::move(update->story_), update->old_story_id_);
      break;
    }
    case td_api::updateStoryPostFailed::ID: {
      auto update = move_object_as<td_api::updateStoryPostFailed>(result);
      on_story_send_failed(update->story_->poster_chat_id_, update->story_->id_, std::move(update->error_));
      break;
    }
    case td_api::updateMessageContent::ID: {
      auto update = move_object_as<td_api::updateMessageContent>(result);
      update_message_content(update->chat_id_, update->message_id_, std::move(update->new_content_));
      break;
    }
    case td_api::updateMessageSuggestedPostInfo::ID: {
      auto update = move_object_as<td_api::updateMessageSuggestedPostInfo>(result);
      on_update_message_suggested_post_info(update->chat_id_, update->message_id_,
                                            std::move(update->suggested_post_info_));
      break;
    }
    case td_api::updateMessageEdited::ID: {
      auto update = move_object_as<td_api::updateMessageEdited>(result);
      auto chat_id = update->chat_id_;
      auto message_id = update->message_id_;
      on_update_message_edited(chat_id, message_id, update->edit_date_, std::move(update->reply_markup_));
      send_request(make_object<td_api::getMessage>(chat_id, message_id),
                   td::make_unique<TdOnGetEditedMessageCallback>(this));
      break;
    }
    case td_api::updateDeleteMessages::ID: {
      auto update = move_object_as<td_api::updateDeleteMessages>(result);
      td::vector<td::unique_ptr<MessageInfo>> deleted_messages;
      for (auto message_id : update->message_ids_) {
        auto deleted_message = delete_message(update->chat_id_, message_id, update->from_cache_);
        if (deleted_message != nullptr) {
          deleted_messages.push_back(std::move(deleted_message));
        }
      }
      td::Scheduler::instance()->destroy_on_scheduler(SharedData::get_file_gc_scheduler_id(), deleted_messages);
      break;
    }
    case td_api::updateFile::ID: {
      auto update = move_object_as<td_api::updateFile>(result);
      on_update_file(std::move(update->file_));
      break;
    }
    case td_api::updateFileGenerationStart::ID: {
      auto update = move_object_as<td_api::updateFileGenerationStart>(result);
      auto generation_id = update->generation_id_;
      send_request(
          make_object<td_api::finishFileGeneration>(generation_id, make_object<td_api::error>(400, "Wrong file_id")),
          td::make_unique<TdOnOkCallback>());
      break;
    }
    case td_api::updateNewChat::ID: {
      auto update = move_object_as<td_api::updateNewChat>(result);
      auto chat = std::move(update->chat_);
      auto chat_info = add_chat(chat->id_);
      switch (chat->type_->get_id()) {
        case td_api::chatTypePrivate::ID: {
          auto type = move_object_as<td_api::chatTypePrivate>(chat->type_);
          chat_info->type = ChatInfo::Type::Private;
          auto user_id = type->user_id_;
          chat_info->user_id = user_id;
          break;
        }
        case td_api::chatTypeBasicGroup::ID: {
          auto type = move_object_as<td_api::chatTypeBasicGroup>(chat->type_);
          chat_info->type = ChatInfo::Type::Group;
          auto group_id = type->basic_group_id_;
          chat_info->group_id = group_id;
          break;
        }
        case td_api::chatTypeSupergroup::ID: {
          auto type = move_object_as<td_api::chatTypeSupergroup>(chat->type_);
          chat_info->type = ChatInfo::Type::Supergroup;
          auto supergroup_id = type->supergroup_id_;
          chat_info->supergroup_id = supergroup_id;
          break;
        }
        case td_api::chatTypeSecret::ID:
          // unsupported
          break;
        default:
          UNREACHABLE();
      }

      chat_info->title = std::move(chat->title_);
      chat_info->photo_info = std::move(chat->photo_);
      chat_info->permissions = std::move(chat->permissions_);
      chat_info->message_auto_delete_time = chat->message_auto_delete_time_;
      chat_info->emoji_status_custom_emoji_id = get_status_custom_emoji_id(chat->emoji_status_);
      chat_info->emoji_status_expiration_date =
          chat->emoji_status_ != nullptr ? chat->emoji_status_->expiration_date_ : 0;
      set_chat_available_reactions(chat_info, std::move(chat->available_reactions_));
      chat_info->accent_color_id = chat->accent_color_id_;
      chat_info->background_custom_emoji_id = chat->background_custom_emoji_id_;
      chat_info->upgraded_gift_colors = std::move(chat->upgraded_gift_colors_);
      chat_info->profile_accent_color_id = chat->profile_accent_color_id_;
      chat_info->profile_background_custom_emoji_id = chat->profile_background_custom_emoji_id_;
      chat_info->has_protected_content = chat->has_protected_content_;
      break;
    }
    case td_api::updateChatTitle::ID: {
      auto update = move_object_as<td_api::updateChatTitle>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      chat_info->title = std::move(update->title_);
      break;
    }
    case td_api::updateChatPhoto::ID: {
      auto update = move_object_as<td_api::updateChatPhoto>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      chat_info->photo_info = std::move(update->photo_);
      break;
    }
    case td_api::updateChatPermissions::ID: {
      auto update = move_object_as<td_api::updateChatPermissions>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      chat_info->permissions = std::move(update->permissions_);
      break;
    }
    case td_api::updateChatMessageAutoDeleteTime::ID: {
      auto update = move_object_as<td_api::updateChatMessageAutoDeleteTime>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      chat_info->message_auto_delete_time = update->message_auto_delete_time_;
      break;
    }
    case td_api::updateChatEmojiStatus::ID: {
      auto update = move_object_as<td_api::updateChatEmojiStatus>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      chat_info->emoji_status_custom_emoji_id = get_status_custom_emoji_id(update->emoji_status_);
      chat_info->emoji_status_expiration_date =
          update->emoji_status_ != nullptr ? update->emoji_status_->expiration_date_ : 0;
      break;
    }
    case td_api::updateChatAvailableReactions::ID: {
      auto update = move_object_as<td_api::updateChatAvailableReactions>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      set_chat_available_reactions(chat_info, std::move(update->available_reactions_));
      break;
    }
    case td_api::updateChatAccentColors::ID: {
      auto update = move_object_as<td_api::updateChatAccentColors>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      chat_info->accent_color_id = update->accent_color_id_;
      chat_info->background_custom_emoji_id = update->background_custom_emoji_id_;
      chat_info->upgraded_gift_colors = std::move(update->upgraded_gift_colors_);
      chat_info->profile_accent_color_id = update->profile_accent_color_id_;
      chat_info->profile_background_custom_emoji_id = update->profile_background_custom_emoji_id_;
      break;
    }
    case td_api::updateChatHasProtectedContent::ID: {
      auto update = move_object_as<td_api::updateChatHasProtectedContent>(result);
      auto chat_info = add_chat(update->chat_id_);
      CHECK(chat_info->type != ChatInfo::Type::Unknown);
      chat_info->has_protected_content = update->has_protected_content_;
      break;
    }
    case td_api::updateUser::ID: {
      auto update = move_object_as<td_api::updateUser>(result);
      auto *user_info = add_user_info(update->user_->id_);
      add_user(user_info, std::move(update->user_));
      break;
    }
    case td_api::updateUserFullInfo::ID: {
      auto update = move_object_as<td_api::updateUserFullInfo>(result);
      auto user_id = update->user_id_;
      auto full_info = update->user_full_info_.get();
      auto user_info = add_user_info(user_id);
      user_info->photo =
          full_info->photo_ == nullptr ? std::move(full_info->public_photo_) : std::move(full_info->photo_);
      user_info->bio = full_info->bio_ != nullptr ? std::move(full_info->bio_->text_) : td::string();
      user_info->birthdate = std::move(full_info->birthdate_);
      user_info->business_info = std::move(full_info->business_info_);
      user_info->accepted_gift_types = std::move(full_info->gift_settings_->accepted_gift_types_);
      user_info->rating = std::move(full_info->rating_);
      user_info->first_profile_audio = std::move(full_info->first_profile_audio_);
      user_info->personal_chat_id = full_info->personal_chat_id_;
      user_info->has_private_forwards = full_info->has_private_forwards_;
      user_info->has_restricted_voice_and_video_messages = full_info->has_restricted_voice_and_video_note_messages_;
      user_info->community_id = full_info->community_id_;
      break;
    }
    case td_api::updateBasicGroup::ID: {
      auto update = move_object_as<td_api::updateBasicGroup>(result);
      auto *group_info = add_group_info(update->basic_group_->id_);
      add_group(group_info, std::move(update->basic_group_));
      break;
    }
    case td_api::updateBasicGroupFullInfo::ID: {
      auto update = move_object_as<td_api::updateBasicGroupFullInfo>(result);
      auto group_id = update->basic_group_id_;
      auto full_info = update->basic_group_full_info_.get();
      auto group_info = add_group_info(group_id);
      group_info->photo = std::move(full_info->photo_);
      group_info->description = std::move(full_info->description_);
      group_info->invite_link =
          full_info->invite_link_ != nullptr ? std::move(full_info->invite_link_->invite_link_) : td::string();
      break;
    }
    case td_api::updateSupergroup::ID: {
      auto update = move_object_as<td_api::updateSupergroup>(result);
      auto *supergroup_info = add_supergroup_info(update->supergroup_->id_);
      add_supergroup(supergroup_info, std::move(update->supergroup_));
      break;
    }
    case td_api::updateCommunity::ID: {
      auto update = move_object_as<td_api::updateCommunity>(result);
      auto *community_info = add_community_info(update->community_->id_);
      add_community(community_info, std::move(update->community_));
      break;
    }
    case td_api::updateSupergroupFullInfo::ID: {
      auto update = move_object_as<td_api::updateSupergroupFullInfo>(result);
      auto supergroup_id = update->supergroup_id_;
      auto full_info = update->supergroup_full_info_.get();
      auto supergroup_info = add_supergroup_info(supergroup_id);
      supergroup_info->photo = std::move(full_info->photo_);
      supergroup_info->description = std::move(full_info->description_);
      supergroup_info->invite_link =
          full_info->invite_link_ != nullptr ? std::move(full_info->invite_link_->invite_link_) : td::string();
      supergroup_info->sticker_set_id = full_info->sticker_set_id_;
      supergroup_info->custom_emoji_sticker_set_id = full_info->custom_emoji_sticker_set_id_;
      supergroup_info->can_set_sticker_set = full_info->can_set_sticker_set_;
      supergroup_info->can_send_gift = full_info->can_send_gift_;
      supergroup_info->is_all_history_available = full_info->is_all_history_available_;
      supergroup_info->slow_mode_delay = full_info->slow_mode_delay_;
      supergroup_info->unrestrict_boost_count = full_info->unrestrict_boost_count_;
      supergroup_info->linked_chat_id = full_info->linked_chat_id_;
      supergroup_info->direct_messages_chat_id = full_info->direct_messages_chat_id_;
      supergroup_info->guard_bot_user_id = full_info->guard_bot_user_id_;
      supergroup_info->community_id = full_info->community_id_;
      supergroup_info->location = std::move(full_info->location_);
      supergroup_info->has_hidden_members = full_info->has_hidden_members_;
      supergroup_info->has_aggressive_anti_spam_enabled = full_info->has_aggressive_anti_spam_enabled_;
      supergroup_info->has_paid_media_allowed = full_info->has_paid_media_allowed_;
      break;
    }
    case td_api::updateOption::ID: {
      auto update = move_object_as<td_api::updateOption>(result);
      const td::string &name = update->name_;
      if (name == "my_id") {
        if (update->value_->get_id() == td_api::optionValueEmpty::ID) {
          CHECK(logging_out_);
          my_id_ = -1;
        } else {
          CHECK(update->value_->get_id() == td_api::optionValueInteger::ID);
          my_id_ = move_object_as<td_api::optionValueInteger>(update->value_)->value_;
        }
      }
      if (name == "group_anonymous_bot_user_id" && update->value_->get_id() == td_api::optionValueInteger::ID) {
        group_anonymous_bot_user_id_ = move_object_as<td_api::optionValueInteger>(update->value_)->value_;
      }
      if (name == "channel_bot_user_id" && update->value_->get_id() == td_api::optionValueInteger::ID) {
        channel_bot_user_id_ = move_object_as<td_api::optionValueInteger>(update->value_)->value_;
      }
      if (name == "telegram_service_notifications_chat_id" &&
          update->value_->get_id() == td_api::optionValueInteger::ID) {
        service_notifications_user_id_ = move_object_as<td_api::optionValueInteger>(update->value_)->value_;
      }
      if (name == "authorization_date") {
        if (update->value_->get_id() == td_api::optionValueEmpty::ID) {
          authorization_date_ = -1;
        } else {
          CHECK(update->value_->get_id() == td_api::optionValueInteger::ID);
          authorization_date_ = static_cast<int32>(move_object_as<td_api::optionValueInteger>(update->value_)->value_);
        }
      }
      if (name == "xallowed_update_types") {
        if (update->value_->get_id() == td_api::optionValueEmpty::ID) {
          allowed_update_types_ = DEFAULT_ALLOWED_UPDATE_TYPES;
        } else {
          CHECK(update->value_->get_id() == td_api::optionValueInteger::ID);
          allowed_update_types_ =
              static_cast<td::uint64>(move_object_as<td_api::optionValueInteger>(update->value_)->value_);
        }
      }
      if (name == "unix_time" && update->value_->get_id() != td_api::optionValueEmpty::ID) {
        CHECK(update->value_->get_id() == td_api::optionValueInteger::ID);
        local_unix_time_difference_ =
            static_cast<double>(move_object_as<td_api::optionValueInteger>(update->value_)->value_) - td::Time::now();
        if (was_authorized_) {
          update_shared_unix_time_difference();
        }
      }
      break;
    }
    case td_api::updatePoll::ID:
      add_update_poll(move_object_as<td_api::updatePoll>(result));
      break;
    case td_api::updatePollAnswer::ID:
      add_update_poll_answer(move_object_as<td_api::updatePollAnswer>(result));
      break;
    case td_api::updateNewInlineQuery::ID: {
      auto update = move_object_as<td_api::updateNewInlineQuery>(result);
      add_new_inline_query(update->id_, update->sender_user_id_, std::move(update->user_location_),
                           std::move(update->chat_type_), update->query_, update->offset_);
      break;
    }
    case td_api::updateNewChosenInlineResult::ID: {
      auto update = move_object_as<td_api::updateNewChosenInlineResult>(result);
      add_new_chosen_inline_result(update->sender_user_id_, std::move(update->user_location_), update->query_,
                                   update->result_id_, update->inline_message_id_);
      break;
    }
    case td_api::updateNewCallbackQuery::ID:
      add_new_callback_query(move_object_as<td_api::updateNewCallbackQuery>(result));
      break;
    case td_api::updateNewBusinessCallbackQuery::ID:
      add_new_business_callback_query(move_object_as<td_api::updateNewBusinessCallbackQuery>(result));
      break;
    case td_api::updateNewInlineCallbackQuery::ID:
      add_new_inline_callback_query(move_object_as<td_api::updateNewInlineCallbackQuery>(result));
      break;
    case td_api::updateNewShippingQuery::ID:
      add_new_shipping_query(move_object_as<td_api::updateNewShippingQuery>(result));
      break;
    case td_api::updateNewPreCheckoutQuery::ID:
      add_new_pre_checkout_query(move_object_as<td_api::updateNewPreCheckoutQuery>(result));
      break;
    case td_api::updatePaidMediaPurchased::ID:
      add_update_purchased_paid_media(move_object_as<td_api::updatePaidMediaPurchased>(result));
      break;
    case td_api::updateManagedBot::ID:
      add_update_managed_bot(move_object_as<td_api::updateManagedBot>(result));
      break;
    case td_api::updateUserSubscription::ID:
      add_update_subscription(move_object_as<td_api::updateUserSubscription>(result));
      break;
    case td_api::updateStopMessageDraft::ID:
      add_update_stopped_message_generation(move_object_as<td_api::updateStopMessageDraft>(result));
      break;
    case td_api::updateNewCustomEvent::ID:
      add_new_custom_event(move_object_as<td_api::updateNewCustomEvent>(result));
      break;
    case td_api::updateNewCustomQuery::ID:
      add_new_custom_query(move_object_as<td_api::updateNewCustomQuery>(result));
      break;
    case td_api::updateChatMember::ID:
      add_update_chat_member(move_object_as<td_api::updateChatMember>(result));
      break;
    case td_api::updateNewChatJoinRequest::ID:
      add_update_chat_join_request(move_object_as<td_api::updateNewChatJoinRequest>(result));
      break;
    case td_api::updateChatBoost::ID:
      add_update_chat_boost(move_object_as<td_api::updateChatBoost>(result));
      break;
    case td_api::updateMessageReaction::ID:
      add_update_message_reaction(move_object_as<td_api::updateMessageReaction>(result));
      break;
    case td_api::updateMessageReactions::ID:
      add_update_message_reaction_count(move_object_as<td_api::updateMessageReactions>(result));
      break;
    case td_api::updateBusinessConnection::ID:
      add_update_business_connection(move_object_as<td_api::updateBusinessConnection>(result));
      break;
    case td_api::updateNewBusinessMessage::ID:
      add_new_business_message(move_object_as<td_api::updateNewBusinessMessage>(result));
      break;
    case td_api::updateBusinessMessageEdited::ID:
      add_business_message_edited(move_object_as<td_api::updateBusinessMessageEdited>(result));
      break;
    case td_api::updateBusinessMessagesDeleted::ID:
      add_update_business_messages_deleted(move_object_as<td_api::updateBusinessMessagesDeleted>(result));
      break;
    case td_api::updateNewGuestQuery::ID:
      add_new_guest_query(move_object_as<td_api::updateNewGuestQuery>(result));
      break;
    case td_api::updateConnectionState::ID: {
      auto update = move_object_as<td_api::updateConnectionState>(result);
      if (update->state_->get_id() == td_api::connectionStateReady::ID) {
        update_last_synchronization_error_date();
        disconnection_time_ = 0;
      } else if (disconnection_time_ == 0) {
        disconnection_time_ = td::Time::now();
      }
      break;
    }
    default:
      // we are not interested in this update
      break;
  }
}

void Client::on_result(td::uint64 id, object_ptr<td_api::Object> result) {
  LOG(DEBUG) << "Receive from Td: " << id << " " << to_string(result);
  if (flood_limited_query_count_ > 0 && td::Time::now() > next_flood_limit_warning_time_) {
    LOG(WARNING) << "Flood-limited " << flood_limited_query_count_ << " queries";
    flood_limited_query_count_ = 0;
    next_flood_limit_warning_time_ = td::Time::now() + 1;
  }

  if (id == 0) {
    return on_update(std::move(result));
  }

  auto *handler_ptr = handlers_.get(id);
  CHECK(handler_ptr != nullptr);
  auto handler = std::move(*handler_ptr);
  handler->on_result(std::move(result));
  handlers_.erase(id);
}

void Client::on_closed() {
  LOG(WARNING) << "Closed";
  CHECK(logging_out_ || closing_);
  CHECK(!td_client_.empty());
  td_client_.reset();

  if (webhook_set_query_) {
    fail_query_closing(std::move(webhook_set_query_));
  }
  if (active_webhook_set_query_) {
    fail_query_closing(std::move(active_webhook_set_query_));
  }
  if (!webhook_url_.empty()) {
    webhook_id_.reset();
  }
  if (long_poll_query_) {
    long_poll_wakeup(true);
    CHECK(!long_poll_query_);
  }

  while (!cmd_queue_.empty()) {
    auto query = std::move(cmd_queue_.front());
    cmd_queue_.pop();
    fail_query_closing(std::move(query));
  }

  while (!pending_send_message_queries_.empty()) {
    auto it = pending_send_message_queries_.begin();
    if (!USE_MESSAGE_DATABASE) {
      LOG(ERROR) << "Doesn't receive updateMessageSendFailed for " << *it->second->query << " with "
                 << it->second->awaited_message_count << " awaited messages";
    }
    fail_query_closing(std::move(it->second->query));
    pending_send_message_queries_.erase(it);
  }
  yet_unsent_message_count_.clear();
  yet_unsent_messages_.clear();

  while (!pending_bot_resolve_queries_.empty()) {
    auto it = pending_bot_resolve_queries_.begin();
    fail_query_closing(std::move(it->second.query));
    pending_bot_resolve_queries_.erase(it);
  }

  while (!file_download_listeners_.empty()) {
    auto it = file_download_listeners_.begin();
    auto file_id = it->first;
    LOG(ERROR) << "Doesn't receive updateFile for file " << file_id;
    auto queries = std::move(it->second);
    file_download_listeners_.erase(it);
    for (auto &query : queries) {
      fail_query_closing(std::move(query));
    }
  }
  download_started_file_ids_.clear();

  if (logging_out_) {
    parameters_->shared_data_->webhook_db_->erase(bot_token_with_dc_);

    td::Scheduler::instance()->run_on_scheduler(
        SharedData::get_file_gc_scheduler_id(),
        [actor_id = actor_id(this), dir = dir_, files_dir = files_dir_](td::Unit) {
          CHECK(dir.size() >= 24);
          CHECK(dir.back() == TD_DIR_SLASH);
          td::rmrf(dir).ignore();
          td::rmrf(files_dir).ignore();
          send_closure(actor_id, &Client::finish_closing);
        });
    return;
  }

  finish_closing();
}

void Client::finish_closing() {
  if (clear_tqueue_ && logging_out_) {
    clear_tqueue();
  }

  if (need_close_) {
    return stop();
  }

  auto timeout = [&] {
    if (next_authorization_time_ <= 0.0) {
      return was_authorized_ && authorization_date_ < get_unix_time() - 1800 ? 1.0 : 1800.0;
    }
    return td::min(next_authorization_time_ - td::Time::now(), 1800.0);
  }();
  set_timeout_in(timeout);
  LOG(INFO) << "Keep client opened for " << timeout << " seconds";
}

void Client::timeout_expired() {
  LOG(WARNING) << "Stop client";
  stop();
}

void Client::clear_tqueue() {
  CHECK(webhook_id_.empty());
  auto &tqueue = parameters_->shared_data_->tqueue_;
  auto deleted_events = tqueue->clear(tqueue_id_, 0);
  td::Scheduler::instance()->destroy_on_scheduler(SharedData::get_file_gc_scheduler_id(), deleted_events);
}

bool Client::to_bool(td::MutableSlice value) {
  td::to_lower_inplace(value);
  value = td::trim(value);
  return value == "true" || value == "yes" || value == "1";
}

template <class T>
td::Result<td::vector<T>> Client::get_array(td::JsonValue &&values, td::Slice class_name,
                                            td::Result<T> (*func)(td::JsonValue &&value)) {
  td::vector<T> items;
  if (values.type() != td::JsonValue::Type::Array) {
    if (values.type() == td::JsonValue::Type::Null) {
      return std::move(items);
    }
    return td::Status::Error(400, PSLICE() << "Expected an Array of " << class_name);
  }

  for (auto &value : values.get_array()) {
    auto r_item = func(std::move(value));
    if (r_item.is_error()) {
      return td::Status::Error(400, PSLICE() << "Can't parse " << class_name << ": " << r_item.error().message());
    }
    items.push_back(r_item.move_as_ok());
  }

  return std::move(items);
}

template <class T>
td::Result<td::vector<td_api::object_ptr<T>>> Client::get_array_generic(
    td::JsonValue &&values, td::Slice class_name,
    const std::function<td::Result<object_ptr<T>>(td::JsonValue &&value)> &func) {
  td::vector<object_ptr<T>> items;
  if (values.type() != td::JsonValue::Type::Array) {
    if (values.type() == td::JsonValue::Type::Null) {
      return std::move(items);
    }
    return td::Status::Error(400, PSLICE() << "Expected an Array of " << class_name);
  }

  for (auto &value : values.get_array()) {
    auto r_item = func(std::move(value));
    if (r_item.is_error()) {
      return td::Status::Error(400, PSLICE() << "Can't parse " << class_name << ": " << r_item.error().message());
    }
    items.push_back(r_item.move_as_ok());
  }

  return std::move(items);
}

td_api::object_ptr<td_api::InputMessageReplyTo> Client::get_input_message_reply_to(
    CheckedReplyParameters &&reply_parameters) {
  if (reply_parameters.reply_to_ephemeral_message_id > 0) {
    return make_object<td_api::inputMessageReplyToEphemeralMessage>(reply_parameters.reply_to_ephemeral_message_id);
  }
  if (reply_parameters.reply_to_message_id > 0) {
    if (reply_parameters.reply_in_chat_id != 0) {
      return make_object<td_api::inputMessageReplyToExternalMessage>(
          reply_parameters.reply_in_chat_id, reply_parameters.reply_to_message_id, std::move(reply_parameters.quote),
          reply_parameters.checklist_task_id, std::move(reply_parameters.poll_option_id));
    }
    return make_object<td_api::inputMessageReplyToMessage>(
        reply_parameters.reply_to_message_id, std::move(reply_parameters.quote), reply_parameters.checklist_task_id,
        std::move(reply_parameters.poll_option_id));
  }
  return nullptr;
}

td_api::object_ptr<td_api::InputMessageReplyTo> Client::get_input_message_reply_to_unchecked(
    InputReplyParameters &&reply_parameters) {
  if (reply_parameters.reply_in_chat_id.empty() && reply_parameters.reply_to_message_id > 0) {
    return make_object<td_api::inputMessageReplyToMessage>(
        reply_parameters.reply_to_message_id, std::move(reply_parameters.quote), reply_parameters.checklist_task_id,
        std::move(reply_parameters.poll_option_id));
  }
  return nullptr;
}

td::Result<Client::InputReplyParameters> Client::get_reply_parameters(const Query *query) {
  if (!query->has_arg("reply_parameters")) {
    InputReplyParameters result;
    result.reply_to_message_id = get_message_id(query, "reply_to_message_id");
    result.allow_sending_without_reply = to_bool(query->arg("allow_sending_without_reply"));
    return std::move(result);
  }

  auto reply_parameters = query->arg("reply_parameters");
  if (reply_parameters.empty()) {
    return InputReplyParameters();
  }

  LOG(INFO) << "Parsing JSON object: " << reply_parameters;
  auto r_value = json_decode(reply_parameters);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse reply parameters JSON object");
  }

  return get_reply_parameters(r_value.move_as_ok());
}

td::Result<Client::InputReplyParameters> Client::get_reply_parameters(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Object expected as reply parameters");
  }
  auto &object = value.get_object();
  if (object.field_count() == 0) {
    return InputReplyParameters();
  }
  TRY_RESULT(chat_id, object.get_optional_string_field("chat_id"));
  TRY_RESULT(message_id, object.get_optional_int_field("message_id"));
  TRY_RESULT(allow_sending_without_reply, object.get_optional_bool_field("allow_sending_without_reply"));
  TRY_RESULT(input_quote, object.get_optional_string_field("quote"));
  TRY_RESULT(parse_mode, object.get_optional_string_field("quote_parse_mode"));
  TRY_RESULT(quote,
             get_formatted_text(std::move(input_quote), std::move(parse_mode), object.extract_field("quote_entities")));
  TRY_RESULT(quote_position, object.get_optional_int_field("quote_position"));
  TRY_RESULT(checklist_task_id, object.get_optional_int_field("checklist_task_id"));
  TRY_RESULT(poll_option_id, object.get_optional_string_field("poll_option_id"));
  TRY_RESULT(ephemeral_message_id, object.get_optional_int_field("ephemeral_message_id"));

  InputReplyParameters result;
  result.reply_in_chat_id = std::move(chat_id);
  result.reply_to_message_id = as_tdlib_message_id(td::max(message_id, 0));
  result.reply_to_ephemeral_message_id = td::max(ephemeral_message_id, 0);
  result.allow_sending_without_reply = allow_sending_without_reply;
  result.quote = make_object<td_api::inputTextQuote>(std::move(quote), quote_position);
  result.checklist_task_id = checklist_task_id;
  result.poll_option_id = std::move(poll_option_id);
  return std::move(result);
}

td::Result<Client::EphemeralMessageParameters> Client::get_ephemeral_message_parameters(const Query *query) {
  if (!query->has_arg("ephemeral_message_parameters")) {
    EphemeralMessageParameters result;
    if (query->has_arg("receiver_user_id")) {
      TRY_RESULT_ASSIGN(result.receiver_user_id, get_user_id(query, "receiver_user_id"));
    }
    result.callback_query_id = td::to_integer<int64>(query->arg("callback_query_id"));
    result.replace_callback_query_message = to_bool(query->arg("replace_callback_query_message"));
    return std::move(result);
  }

  auto ephemeral_message_parameters = query->arg("ephemeral_message_parameters");
  if (ephemeral_message_parameters.empty()) {
    return EphemeralMessageParameters();
  }

  LOG(INFO) << "Parsing JSON object: " << ephemeral_message_parameters;
  auto r_value = json_decode(ephemeral_message_parameters);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse EphemeralMessageParameters JSON object");
  }

  return get_ephemeral_message_parameters(r_value.move_as_ok());
}

td::Result<Client::EphemeralMessageParameters> Client::get_ephemeral_message_parameters(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Object expected as EphemeralMessageParameters");
  }
  auto &object = value.get_object();
  if (object.field_count() == 0) {
    return EphemeralMessageParameters();
  }
  EphemeralMessageParameters result;
  TRY_RESULT_ASSIGN(result.receiver_user_id, object.get_required_long_field("receiver_user_id"));
  TRY_RESULT_ASSIGN(result.callback_query_id, object.get_optional_long_field("callback_query_id"));
  TRY_RESULT_ASSIGN(result.replace_callback_query_message,
                    object.get_optional_bool_field("replace_callback_query_message"));
  return std::move(result);
}

td::Result<td_api::object_ptr<td_api::ButtonStyle>> Client::get_button_style(td::Result<td::string> r_style,
                                                                             bool for_rich_message) {
  TRY_RESULT(style, std::move(r_style));
  td::to_lower_inplace(style);
  if (style.empty() || style == "default") {
    return make_object<td_api::buttonStyleDefault>();
  }
  if (style == "primary") {
    return make_object<td_api::buttonStylePrimary>();
  }
  if (style == "danger") {
    return make_object<td_api::buttonStyleDanger>();
  }
  if (style == "success") {
    return make_object<td_api::buttonStyleSuccess>();
  }
  if (for_rich_message && style == "link") {
    return make_object<td_api::buttonStyleLink>();
  }
  return td::Status::Error("Invalid button style specified");
}

td::Result<td_api::object_ptr<td_api::KeyboardButtonType>> Client::get_keyboard_button_type(td::JsonObject &object) {
  TRY_RESULT(request_phone_number, object.get_optional_bool_field("request_phone_number"));
  TRY_RESULT(request_contact, object.get_optional_bool_field("request_contact"));
  if (request_phone_number || request_contact) {
    return make_object<td_api::keyboardButtonTypeRequestPhoneNumber>();
  }

  TRY_RESULT(request_location, object.get_optional_bool_field("request_location"));
  if (request_location) {
    return make_object<td_api::keyboardButtonTypeRequestLocation>();
  }

  if (object.has_field("request_poll")) {
    bool force_regular = false;
    bool force_quiz = false;
    TRY_RESULT(request_poll, object.extract_required_field("request_poll", td::JsonValue::Type::Object));
    auto &request_poll_object = request_poll.get_object();
    if (request_poll_object.has_field("type")) {
      TRY_RESULT(type, request_poll_object.get_optional_string_field("type"));
      if (type == "quiz") {
        force_quiz = true;
      } else if (type == "regular") {
        force_regular = true;
      }
    }
    return make_object<td_api::keyboardButtonTypeRequestPoll>(force_regular, force_quiz);
  }

  if (object.has_field("web_app")) {
    TRY_RESULT(web_app, object.extract_required_field("web_app", td::JsonValue::Type::Object));
    auto &web_app_object = web_app.get_object();
    TRY_RESULT(url, web_app_object.get_required_string_field("url"));
    return make_object<td_api::keyboardButtonTypeWebApp>(url);
  }

  if (object.has_field("request_user") || object.has_field("request_users")) {
    td::JsonValue request_user;
    if (object.has_field("request_users")) {
      TRY_RESULT_ASSIGN(request_user, object.extract_required_field("request_users", td::JsonValue::Type::Object));
    } else {
      TRY_RESULT_ASSIGN(request_user, object.extract_required_field("request_user", td::JsonValue::Type::Object));
    }
    auto &request_user_object = request_user.get_object();
    TRY_RESULT(id, request_user_object.get_required_int_field("request_id"));
    auto restrict_user_is_bot = request_user_object.has_field("user_is_bot");
    TRY_RESULT(user_is_bot, request_user_object.get_optional_bool_field("user_is_bot"));
    auto restrict_user_is_premium = request_user_object.has_field("user_is_premium");
    TRY_RESULT(user_is_premium, request_user_object.get_optional_bool_field("user_is_premium"));
    TRY_RESULT(max_quantity, request_user_object.get_optional_int_field("max_quantity", 1));
    TRY_RESULT(request_name, request_user_object.get_optional_bool_field("request_name"));
    TRY_RESULT(request_username, request_user_object.get_optional_bool_field("request_username"));
    TRY_RESULT(request_photo, request_user_object.get_optional_bool_field("request_photo"));
    return make_object<td_api::keyboardButtonTypeRequestUsers>(id, restrict_user_is_bot, user_is_bot,
                                                               restrict_user_is_premium, user_is_premium, max_quantity,
                                                               request_name, request_username, request_photo);
  }

  if (object.has_field("request_chat")) {
    TRY_RESULT(request_chat, object.extract_required_field("request_chat", td::JsonValue::Type::Object));
    auto &request_chat_object = request_chat.get_object();
    TRY_RESULT(id, request_chat_object.get_required_int_field("request_id"));
    TRY_RESULT(chat_is_channel, request_chat_object.get_optional_bool_field("chat_is_channel"));
    auto restrict_chat_is_forum = request_chat_object.has_field("chat_is_forum");
    TRY_RESULT(chat_is_forum, request_chat_object.get_optional_bool_field("chat_is_forum"));
    auto restrict_chat_has_username = request_chat_object.has_field("chat_has_username");
    TRY_RESULT(chat_has_username, request_chat_object.get_optional_bool_field("chat_has_username"));
    TRY_RESULT(chat_is_created, request_chat_object.get_optional_bool_field("chat_is_created"));
    object_ptr<td_api::chatAdministratorRights> user_administrator_rights;
    if (request_chat_object.has_field("user_administrator_rights")) {
      TRY_RESULT_ASSIGN(user_administrator_rights,
                        get_chat_administrator_rights(request_chat_object.extract_field("user_administrator_rights")));
    }
    object_ptr<td_api::chatAdministratorRights> bot_administrator_rights;
    if (request_chat_object.has_field("bot_administrator_rights")) {
      TRY_RESULT_ASSIGN(bot_administrator_rights,
                        get_chat_administrator_rights(request_chat_object.extract_field("bot_administrator_rights")));
    }
    TRY_RESULT(bot_is_member, request_chat_object.get_optional_bool_field("bot_is_member"));
    TRY_RESULT(request_title, request_chat_object.get_optional_bool_field("request_title"));
    TRY_RESULT(request_username, request_chat_object.get_optional_bool_field("request_username"));
    TRY_RESULT(request_photo, request_chat_object.get_optional_bool_field("request_photo"));
    return make_object<td_api::keyboardButtonTypeRequestChat>(
        id, chat_is_channel, restrict_chat_is_forum, chat_is_forum, restrict_chat_has_username, chat_has_username,
        chat_is_created, std::move(user_administrator_rights), std::move(bot_administrator_rights), bot_is_member,
        request_title, request_username, request_photo);
  }

  if (object.has_field("request_managed_bot")) {
    TRY_RESULT(request_managed_bot, object.extract_required_field("request_managed_bot", td::JsonValue::Type::Object));
    auto &request_managed_bot_object = request_managed_bot.get_object();
    TRY_RESULT(id, request_managed_bot_object.get_required_int_field("request_id"));
    TRY_RESULT(suggested_name, request_managed_bot_object.get_optional_string_field("suggested_name"));
    TRY_RESULT(suggested_username, request_managed_bot_object.get_optional_string_field("suggested_username"));
    return make_object<td_api::keyboardButtonTypeRequestManagedBot>(id, suggested_name, suggested_username);
  }

  return nullptr;
}

td::Result<td_api::object_ptr<td_api::keyboardButton>> Client::get_keyboard_button(td::JsonValue &&button) {
  if (button.type() == td::JsonValue::Type::Object) {
    auto &object = button.get_object();

    TRY_RESULT(text, object.get_required_string_field("text"));
    TRY_RESULT(icon, object.get_optional_long_field("icon_custom_emoji_id"));
    TRY_RESULT(style, get_button_style(object.get_optional_string_field("style"), false));
    TRY_RESULT(type, get_keyboard_button_type(object));
    return make_object<td_api::keyboardButton>(text, std::move(icon), std::move(style), std::move(type));
  }
  if (button.type() == td::JsonValue::Type::String) {
    return make_object<td_api::keyboardButton>(button.get_string().str(), 0, nullptr, nullptr);
  }

  return td::Status::Error(400, "KeyboardButton must be a String or an Object");
}

td::Result<td_api::object_ptr<td_api::inlineKeyboardButton>> Client::get_inline_keyboard_button(
    td::JsonValue &&button, BotUserIds &bot_user_ids) {
  if (button.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "InlineKeyboardButton must be an Object");
  }

  auto &object = button.get_object();
  TRY_RESULT(text, object.get_required_string_field("text"));
  TRY_RESULT(icon, object.get_optional_long_field("icon_custom_emoji_id"));
  TRY_RESULT(style, get_button_style(object.get_optional_string_field("style"), false));
  TRY_RESULT(type, get_inline_keyboard_button_type(object, &bot_user_ids));
  return make_object<td_api::inlineKeyboardButton>(text, std::move(icon), std::move(style), std::move(type));
}

td::Result<td_api::object_ptr<td_api::inlineButton>> Client::get_inline_button(td::JsonValue &&button) {
  if (button.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "InlineButton must be an Object");
  }

  auto &object = button.get_object();
  TRY_RESULT(text, get_rich_text(object.extract_field("text")));
  TRY_RESULT(style, get_button_style(object.get_optional_string_field("style"), true));
  TRY_RESULT(type, get_inline_keyboard_button_type(object, nullptr));
  return make_object<td_api::inlineButton>(std::move(text), std::move(style), std::move(type));
}

td::Result<td_api::object_ptr<td_api::InlineKeyboardButtonType>> Client::get_inline_keyboard_button_type(
    td::JsonObject &object, BotUserIds *bot_user_ids) {
  {
    TRY_RESULT(url, object.get_optional_string_field("url"));
    if (!url.empty()) {
      return make_object<td_api::inlineKeyboardButtonTypeUrl>(url);
    }
  }

  {
    TRY_RESULT(callback_data, object.get_optional_string_field("callback_data"));
    if (!callback_data.empty()) {
      return make_object<td_api::inlineKeyboardButtonTypeCallback>(callback_data);
    }
  }

  if (object.has_field("callback_game")) {
    return make_object<td_api::inlineKeyboardButtonTypeCallbackGame>();
  }

  if (object.has_field("pay")) {
    return make_object<td_api::inlineKeyboardButtonTypeBuy>();
  }

  if (object.has_field("switch_inline_query")) {
    TRY_RESULT(switch_inline_query, object.get_required_string_field("switch_inline_query"));
    return make_object<td_api::inlineKeyboardButtonTypeSwitchInline>(
        switch_inline_query,
        make_object<td_api::targetChatChosen>(make_object<td_api::targetChatTypes>(true, true, true, true)));
  }

  if (object.has_field("switch_inline_query_chosen_chat")) {
    TRY_RESULT(switch_inline_query,
               object.extract_required_field("switch_inline_query_chosen_chat", td::JsonValue::Type::Object));
    CHECK(switch_inline_query.type() == td::JsonValue::Type::Object);
    auto &switch_inline_query_object = switch_inline_query.get_object();
    TRY_RESULT(query, switch_inline_query_object.get_optional_string_field("query"));
    TRY_RESULT(allow_user_chats, switch_inline_query_object.get_optional_bool_field("allow_user_chats"));
    TRY_RESULT(allow_bot_chats, switch_inline_query_object.get_optional_bool_field("allow_bot_chats"));
    TRY_RESULT(allow_group_chats, switch_inline_query_object.get_optional_bool_field("allow_group_chats"));
    TRY_RESULT(allow_channel_chats, switch_inline_query_object.get_optional_bool_field("allow_channel_chats"));
    return make_object<td_api::inlineKeyboardButtonTypeSwitchInline>(
        query, make_object<td_api::targetChatChosen>(make_object<td_api::targetChatTypes>(
                   allow_user_chats, allow_bot_chats, allow_group_chats, allow_channel_chats)));
  }

  if (object.has_field("switch_inline_query_current_chat")) {
    TRY_RESULT(switch_inline_query, object.get_required_string_field("switch_inline_query_current_chat"));
    return make_object<td_api::inlineKeyboardButtonTypeSwitchInline>(switch_inline_query,
                                                                     make_object<td_api::targetChatCurrent>());
  }

  if (object.has_field("login_url")) {
    TRY_RESULT(login_url, object.extract_required_field("login_url", td::JsonValue::Type::Object));
    CHECK(login_url.type() == td::JsonValue::Type::Object);
    auto &login_url_object = login_url.get_object();
    TRY_RESULT(url, login_url_object.get_required_string_field("url"));
    TRY_RESULT(bot_username, login_url_object.get_optional_string_field("bot_username"));
    TRY_RESULT(request_write_access, login_url_object.get_optional_bool_field("request_write_access"));
    TRY_RESULT(forward_text, login_url_object.get_optional_string_field("forward_text"));

    int64 bot_user_id = 0;
    if (!bot_username.empty()) {
      if (bot_user_ids == nullptr) {
        return td::Status::Error(400, "Bot username must be empty for login_url buttons in rich messages");
      }
      if (bot_username[0] == '@') {
        bot_username = bot_username.substr(1);
      }
      if (bot_username.empty()) {
        return td::Status::Error(400, "LoginUrl bot username is invalid");
      }
      for (auto c : bot_username) {
        if (c != '_' && !td::is_alnum(c)) {
          return td::Status::Error(400, "LoginUrl bot username is invalid");
        }
      }
      auto &user_id = bot_user_ids->bot_user_ids_[bot_username];
      if (user_id == 0) {
        user_id = bot_user_ids->cur_temp_bot_user_id_++;
        user_id *= 1000;
      }
      if (user_id % 1000 == 0) {
        bot_user_ids->unresolved_bot_usernames_.insert(bot_username);
      }
      bot_user_id = user_id;
    }
    if (!request_write_access) {
      bot_user_id = -bot_user_id - 1;
    }
    return make_object<td_api::inlineKeyboardButtonTypeLoginUrl>(url, bot_user_id, forward_text);
  }

  if (object.has_field("web_app")) {
    TRY_RESULT(web_app, object.extract_required_field("web_app", td::JsonValue::Type::Object));
    auto &web_app_object = web_app.get_object();
    TRY_RESULT(url, web_app_object.get_required_string_field("url"));
    return make_object<td_api::inlineKeyboardButtonTypeWebApp>(url);
  }

  if (object.has_field("copy_text")) {
    TRY_RESULT(copy_text, object.extract_required_field("copy_text", td::JsonValue::Type::Object));
    auto &copy_text_object = copy_text.get_object();
    TRY_RESULT(copied_text, copy_text_object.get_required_string_field("text"));
    return make_object<td_api::inlineKeyboardButtonTypeCopyText>(copied_text);
  }

  if (object.has_field("disabled")) {
    return make_object<td_api::inlineKeyboardButtonTypeDisabled>();
  }

  return td::Status::Error(400, "Text buttons are not allowed in the inline keyboard");
}

td::Result<td_api::object_ptr<td_api::ReplyMarkup>> Client::get_reply_markup(const Query *query,
                                                                             BotUserIds &bot_user_ids) {
  auto reply_markup = query->arg("reply_markup");
  if (reply_markup.empty()) {
    return nullptr;
  }

  LOG(INFO) << "Parsing JSON object: " << reply_markup;
  auto r_value = json_decode(reply_markup);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse reply keyboard markup JSON object");
  }

  return get_reply_markup(r_value.move_as_ok(), bot_user_ids);
}

td::Result<td_api::object_ptr<td_api::ReplyMarkup>> Client::get_reply_markup(td::JsonValue &&value,
                                                                             BotUserIds &bot_user_ids) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Object expected as reply markup");
  }
  auto &object = value.get_object();

  td::vector<td::vector<object_ptr<td_api::keyboardButton>>> rows;
  TRY_RESULT(keyboard, object.extract_optional_field("keyboard", td::JsonValue::Type::Array));
  if (keyboard.type() == td::JsonValue::Type::Array) {
    for (auto &row : keyboard.get_array()) {
      TRY_RESULT(new_row, get_array(std::move(row), "KeyboardButton", get_keyboard_button));
      rows.push_back(std::move(new_row));
    }
  }

  td::vector<td::vector<object_ptr<td_api::inlineKeyboardButton>>> inline_rows;
  TRY_RESULT(inline_keyboard, object.extract_optional_field("inline_keyboard", td::JsonValue::Type::Array));
  if (inline_keyboard.type() == td::JsonValue::Type::Array) {
    for (auto &row : inline_keyboard.get_array()) {
      TRY_RESULT(new_row, get_array_generic<td_api::inlineKeyboardButton>(
                              std::move(row), "InlineKeyboardButton", [&](td::JsonValue &&button) {
                                return get_inline_keyboard_button(std::move(button), bot_user_ids);
                              }));
      inline_rows.push_back(std::move(new_row));
    }
  }

  TRY_RESULT(hide_keyboard, object.get_optional_bool_field("hide_keyboard"));
  TRY_RESULT(remove_keyboard, object.get_optional_bool_field("remove_keyboard"));
  TRY_RESULT(personal_keyboard, object.get_optional_bool_field("personal_keyboard"));
  TRY_RESULT(selective, object.get_optional_bool_field("selective"));
  TRY_RESULT(force_reply_keyboard, object.get_optional_bool_field("force_reply_keyboard"));
  TRY_RESULT(force_reply, object.get_optional_bool_field("force_reply"));
  TRY_RESULT(input_field_placeholder, object.get_optional_string_field("input_field_placeholder"));
  bool is_personal = personal_keyboard || selective;

  object_ptr<td_api::ReplyMarkup> result;
  if (!rows.empty()) {
    TRY_RESULT(resize_keyboard, object.get_optional_bool_field("resize_keyboard"));
    TRY_RESULT(one_time_keyboard, object.get_optional_bool_field("one_time_keyboard"));
    TRY_RESULT(is_persistent, object.get_optional_bool_field("is_persistent"));
    result =
        make_object<td_api::replyMarkupShowKeyboard>(std::move(rows), is_persistent, resize_keyboard, one_time_keyboard,
                                                     is_personal, force_reply, input_field_placeholder);
  } else if (!inline_rows.empty()) {
    result = make_object<td_api::replyMarkupInlineKeyboard>(std::move(inline_rows), force_reply);
  } else if (hide_keyboard || remove_keyboard) {
    result = make_object<td_api::replyMarkupRemoveKeyboard>(is_personal);
  } else if (force_reply || force_reply_keyboard) {
    result = make_object<td_api::replyMarkupForceReply>(is_personal, input_field_placeholder);
  }
  if (result == nullptr || result->get_id() != td_api::replyMarkupInlineKeyboard::ID) {
    bot_user_ids.unresolved_bot_usernames_.clear();
  }

  return std::move(result);
}

td::Result<td_api::object_ptr<td_api::SuggestedPostPrice>> Client::get_suggested_post_price(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Object expected as suggested post price");
  }
  auto &object = value.get_object();

  TRY_RESULT(currency, object.get_required_string_field("currency"));
  TRY_RESULT(amount, object.get_required_long_field("amount"));

  if (currency == "XTR") {
    return make_object<td_api::suggestedPostPriceStar>(amount);
  }
  if (currency == "TON") {
    if (amount % 10000000 != 0) {
      return td::Status::Error(400, "Suggested post price must be divisible by 10000000 nanograms");
    }
    return make_object<td_api::suggestedPostPriceGram>(amount / 10000000);
  }
  return td::Status::Error(400, "Invalid suggested post price currency specified");
}

td::Result<td_api::object_ptr<td_api::inputSuggestedPostInfo>> Client::get_input_suggested_post_info(
    const Query *query) {
  auto suggested_post = query->arg("suggested_post_parameters");
  if (suggested_post.empty()) {
    return nullptr;
  }

  LOG(INFO) << "Parsing JSON object: " << suggested_post;
  auto r_value = json_decode(suggested_post);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse suggested post parameters JSON object");
  }

  return get_input_suggested_post_info(r_value.move_as_ok());
}

td::Result<td_api::object_ptr<td_api::inputSuggestedPostInfo>> Client::get_input_suggested_post_info(
    td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Object expected as suggested post parameters");
  }
  auto &object = value.get_object();

  TRY_RESULT(send_date, object.get_optional_int_field("send_date"));

  object_ptr<td_api::SuggestedPostPrice> price;
  TRY_RESULT(input_price, object.extract_optional_field("price", td::JsonValue::Type::Object));
  if (input_price.type() == td::JsonValue::Type::Object) {
    TRY_RESULT_ASSIGN(price, get_suggested_post_price(std::move(input_price)));
  }
  return make_object<td_api::inputSuggestedPostInfo>(std::move(price), send_date);
}

td::Result<td_api::object_ptr<td_api::labeledPricePart>> Client::get_labeled_price_part(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "LabeledPrice must be an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(label, object.get_required_string_field("label"));
  if (label.empty()) {
    return td::Status::Error(400, "LabeledPrice label must be non-empty");
  }

  TRY_RESULT(amount, object.get_required_long_field("amount"));
  return make_object<td_api::labeledPricePart>(label, amount);
}

td::Result<td::vector<td_api::object_ptr<td_api::labeledPricePart>>> Client::get_labeled_price_parts(
    td::JsonValue &&value) {
  return get_array(std::move(value), "LabeledPrice", get_labeled_price_part);
}

td::Result<td::int64> Client::get_suggested_tip_amount(td::JsonValue &&value) {
  td::Slice number;
  if (value.type() == td::JsonValue::Type::Number) {
    number = value.get_number();
  } else if (value.type() == td::JsonValue::Type::String) {
    number = value.get_string();
  } else {
    return td::Status::Error(400, "Suggested tip amount must be of type Number or String");
  }
  auto parsed_amount = td::to_integer_safe<int64>(number);
  if (parsed_amount.is_error()) {
    return td::Status::Error(400, "Can't parse suggested tip amount as Number");
  }
  return parsed_amount.ok();
}

td::Result<td::vector<td::int64>> Client::get_suggested_tip_amounts(td::JsonValue &&value) {
  return get_array(std::move(value), "suggested tip amount", get_suggested_tip_amount);
}

td::Result<td_api::object_ptr<td_api::shippingOption>> Client::get_shipping_option(td::JsonValue &&option) {
  if (option.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "ShippingOption must be an Object");
  }

  auto &object = option.get_object();

  TRY_RESULT(id, object.get_required_string_field("id"));
  if (id.empty()) {
    return td::Status::Error(400, "ShippingOption identifier must be non-empty");
  }

  TRY_RESULT(title, object.get_required_string_field("title"));
  if (title.empty()) {
    return td::Status::Error(400, "ShippingOption title must be non-empty");
  }

  TRY_RESULT(prices_json, object.extract_required_field("prices", td::JsonValue::Type::Array));

  auto r_prices = get_labeled_price_parts(std::move(prices_json));
  if (r_prices.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse shipping option prices: " << r_prices.error().message());
  }

  return make_object<td_api::shippingOption>(id, title, r_prices.move_as_ok());
}

td::Result<td::vector<td_api::object_ptr<td_api::shippingOption>>> Client::get_shipping_options(const Query *query) {
  TRY_RESULT(shipping_options, get_required_string_arg(query, "shipping_options"));

  LOG(INFO) << "Parsing JSON object: " << shipping_options;
  auto r_value = json_decode(shipping_options);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse shipping options JSON object");
  }

  return get_array(r_value.move_as_ok(), "ShippingOption", get_shipping_option);
}

td_api::object_ptr<td_api::ChatAction> Client::get_chat_action(const Query *query) {
  auto action = query->arg("action");
  td::to_lower_inplace(action);
  if (action == "cancel") {
    return make_object<td_api::chatActionCancel>();
  }
  if (action == "typing") {
    return make_object<td_api::chatActionTyping>();
  }
  if (action == "record_video") {
    return make_object<td_api::chatActionRecordingVideo>();
  }
  if (action == "upload_video") {
    return make_object<td_api::chatActionUploadingVideo>();
  }
  if (action == "record_audio" || action == "record_voice") {
    return make_object<td_api::chatActionRecordingVoiceNote>();
  }
  if (action == "upload_audio" || action == "upload_voice") {
    return make_object<td_api::chatActionUploadingVoiceNote>();
  }
  if (action == "upload_photo") {
    return make_object<td_api::chatActionUploadingPhoto>();
  }
  if (action == "upload_document") {
    return make_object<td_api::chatActionUploadingDocument>();
  }
  if (action == "choose_sticker") {
    return make_object<td_api::chatActionChoosingSticker>();
  }
  if (action == "pick_up_location" || action == "find_location") {
    return make_object<td_api::chatActionChoosingLocation>();
  }
  if (action == "record_video_note") {
    return make_object<td_api::chatActionRecordingVideoNote>();
  }
  if (action == "upload_video_note") {
    return make_object<td_api::chatActionUploadingVideoNote>();
  }
  return nullptr;
}

td_api::object_ptr<td_api::InputFile> Client::get_input_file(const Query *query, td::Slice field_name,
                                                             bool force_file) const {
  return get_input_file(query, field_name, query->arg(field_name), force_file);
}

td::string Client::get_local_file_path(td::Slice file_uri) {
  if (td::begins_with(file_uri, "/")) {
    file_uri.remove_prefix(td::begins_with(file_uri, "/localhost") ? 10 : 1);
  }
#if TD_PORT_WINDOWS
  if (td::begins_with(file_uri, "/")) {
    file_uri.remove_prefix(1);
  }
#endif
  td::string result(file_uri.size(), '\0');
  auto result_len = url_decode(file_uri, result, false);
  result.resize(result_len);
  return result;
}

td_api::object_ptr<td_api::InputFile> Client::get_input_file(const Query *query, td::Slice field_name,
                                                             td::Slice file_id, bool force_file) const {
  if (!file_id.empty()) {
    if (parameters_->local_mode_) {
      td::Slice file_protocol{"file:/"};
      if (td::begins_with(file_id, file_protocol)) {
        return make_object<td_api::inputFileLocal>(get_local_file_path(file_id.substr(file_protocol.size())));
      }
    }
    td::Slice attach_protocol{"attach://"};
    if (td::begins_with(file_id, attach_protocol)) {
      field_name = file_id.substr(attach_protocol.size());
    } else {
      if (!force_file) {
        return make_object<td_api::inputFileRemote>(file_id.str());
      }
    }
  }
  auto file = query->file(field_name);
  if (file != nullptr) {
    return make_object<td_api::inputFileLocal>(file->temp_file_name);
  }

  return nullptr;
}

td_api::object_ptr<td_api::inputThumbnail> Client::get_input_thumbnail(const Query *query) const {
  auto input_file = get_input_file(query, "thumbnail", true);
  if (input_file == nullptr) {
    input_file = get_input_file(query, "thumb", true);
    if (input_file == nullptr) {
      return nullptr;
    }
  }
  return make_object<td_api::inputThumbnail>(std::move(input_file), 0, 0);
}

td::Result<td_api::object_ptr<td_api::inputThumbnail>> Client::get_input_thumbnail(const Query *query,
                                                                                   const td::JsonObject &object,
                                                                                   bool allow_legacy) const {
  TRY_RESULT(thumbnail, object.get_optional_string_field("thumbnail"));
  if (allow_legacy) {
    if (thumbnail.empty()) {
      TRY_RESULT_ASSIGN(thumbnail, object.get_optional_string_field("thumb"));
    }
    auto thumbnail_input_file = get_input_file(query, td::Slice(), thumbnail, true);
    if (thumbnail_input_file == nullptr) {
      thumbnail_input_file = get_input_file(query, "thumbnail", td::Slice(), true);
      if (thumbnail_input_file == nullptr) {
        thumbnail_input_file = get_input_file(query, "thumb", td::Slice(), true);
      }
    }
    if (thumbnail_input_file != nullptr) {
      return make_object<td_api::inputThumbnail>(std::move(thumbnail_input_file), 0, 0);
    }
  } else {
    auto thumbnail_input_file = get_input_file(query, "thumbnail", thumbnail, true);
    if (thumbnail_input_file != nullptr) {
      return make_object<td_api::inputThumbnail>(std::move(thumbnail_input_file), 0, 0);
    }
  }
  return nullptr;
}

td::Result<td_api::object_ptr<td_api::InputMessageContent>> Client::get_input_message_content(
    const Query *query, td::JsonObject &object, bool is_input_message_content_required) const {
  if (object.has_field("rich_message")) {
    TRY_RESULT(rich_message, object.extract_required_field("rich_message", td::JsonValue::Type::Object));
    TRY_RESULT(input_rich_message, get_input_rich_message(query, std::move(rich_message)));
    return make_object<td_api::inputMessageRichMessage>(std::move(input_rich_message), false);
  }

  TRY_RESULT(message_text, object.get_optional_string_field("message_text"));

  if (!message_text.empty()) {
    object_ptr<td_api::linkPreviewOptions> link_preview_options;
    if (object.has_field("link_preview_options")) {
      TRY_RESULT(options, object.extract_required_field("link_preview_options", td::JsonValue::Type::Object));
      CHECK(options.type() == td::JsonValue::Type::Object);
      TRY_RESULT_ASSIGN(link_preview_options, get_link_preview_options(std::move(options)));
    } else {
      TRY_RESULT(disable_web_page_preview, object.get_optional_bool_field("disable_web_page_preview"));
      link_preview_options = get_link_preview_options(disable_web_page_preview);
    }
    TRY_RESULT(parse_mode, object.get_optional_string_field("parse_mode"));
    auto entities = object.extract_field("entities");
    TRY_RESULT(input_message_text, get_input_message_text(std::move(message_text), std::move(link_preview_options),
                                                          std::move(parse_mode), std::move(entities)));
    return std::move(input_message_text);
  }

  if (object.has_field("latitude") && object.has_field("longitude")) {
    if (object.has_field("title") && object.has_field("address")) {
      TRY_RESULT(venue, get_venue(object));
      return make_object<td_api::inputMessageVenue>(std::move(venue));
    }

    TRY_RESULT(location, get_location(object));
    TRY_RESULT(live_period, object.get_optional_int_field("live_period"));
    TRY_RESULT(heading, object.get_optional_int_field("heading"));
    TRY_RESULT(proximity_alert_radius, object.get_optional_int_field("proximity_alert_radius"));
    if (live_period != 0) {
      return make_object<td_api::inputMessageLiveLocation>(
          make_object<td_api::liveLocation>(std::move(location), live_period, heading, proximity_alert_radius));
    } else {
      return make_object<td_api::inputMessageLocation>(std::move(location));
    }
  }

  if (object.has_field("phone_number")) {
    TRY_RESULT(phone_number, object.get_required_string_field("phone_number"));
    TRY_RESULT(first_name, object.get_required_string_field("first_name"));
    TRY_RESULT(last_name, object.get_optional_string_field("last_name"));
    TRY_RESULT(vcard, object.get_optional_string_field("vcard"));

    return make_object<td_api::inputMessageContact>(
        make_object<td_api::contact>(phone_number, first_name, last_name, vcard, 0));
  }

  if (object.has_field("payload")) {
    TRY_RESULT(title, object.get_required_string_field("title"));
    TRY_RESULT(description, object.get_required_string_field("description"));
    TRY_RESULT(payload, object.get_required_string_field("payload"));
    if (!td::check_utf8(payload)) {
      return td::Status::Error(400, "InputInvoiceMessageContent payload must be encoded in UTF-8");
    }
    TRY_RESULT(provider_token, object.get_optional_string_field("provider_token"));
    TRY_RESULT(currency, object.get_required_string_field("currency"));
    TRY_RESULT(prices_object, object.extract_required_field("prices", td::JsonValue::Type::Array));
    TRY_RESULT(prices, get_labeled_price_parts(std::move(prices_object)));
    TRY_RESULT(provider_data, object.get_optional_string_field("provider_data"));
    TRY_RESULT(max_tip_amount, object.get_optional_long_field("max_tip_amount"));
    TRY_RESULT(suggested_tip_amounts, get_suggested_tip_amounts(object.extract_field("suggested_tip_amounts")));
    TRY_RESULT(photo_url, object.get_optional_string_field("photo_url"));
    TRY_RESULT(photo_size, object.get_optional_int_field("photo_size"));
    TRY_RESULT(photo_width, object.get_optional_int_field("photo_width"));
    TRY_RESULT(photo_height, object.get_optional_int_field("photo_height"));
    TRY_RESULT(need_name, object.get_optional_bool_field("need_name"));
    TRY_RESULT(need_phone_number, object.get_optional_bool_field("need_phone_number"));
    TRY_RESULT(need_email_address, object.get_optional_bool_field("need_email"));
    TRY_RESULT(need_shipping_address, object.get_optional_bool_field("need_shipping_address"));
    TRY_RESULT(send_phone_number_to_provider, object.get_optional_bool_field("send_phone_number_to_provider"));
    TRY_RESULT(send_email_address_to_provider, object.get_optional_bool_field("send_email_to_provider"));
    TRY_RESULT(is_flexible, object.get_optional_bool_field("is_flexible"));

    return make_object<td_api::inputMessageInvoice>(
        make_object<td_api::invoice>(currency, std::move(prices), 0, max_tip_amount, std::move(suggested_tip_amounts),
                                     td::string(), td::string(), false, need_name, need_phone_number,
                                     need_email_address, need_shipping_address, send_phone_number_to_provider,
                                     send_email_address_to_provider, is_flexible),
        title, description, photo_url, photo_size, photo_width, photo_height, payload, provider_token, provider_data,
        td::string(), nullptr, nullptr);
  }

  if (is_input_message_content_required) {
    return td::Status::Error(400, "Input message content is not specified");
  }
  return nullptr;
}

td_api::object_ptr<td_api::messageSendOptions> Client::get_message_send_options(
    bool disable_notification, bool protect_content, bool allow_paid_broadcast, int64 effect_id,
    object_ptr<td_api::inputSuggestedPostInfo> &&input_suggested_post_info) {
  return make_object<td_api::messageSendOptions>(std::move(input_suggested_post_info), disable_notification, false,
                                                 protect_content, allow_paid_broadcast, 0, false, nullptr, effect_id, 0,
                                                 false);
}

td::Result<td_api::object_ptr<td_api::inlineQueryResultsButton>> Client::get_inline_query_results_button(
    td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "InlineQueryResultsButton must be an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(text, object.get_required_string_field("text"));

  if (object.has_field("start_parameter")) {
    TRY_RESULT(start_parameter, object.get_required_string_field("start_parameter"));
    return make_object<td_api::inlineQueryResultsButton>(
        text, make_object<td_api::inlineQueryResultsButtonTypeStartBot>(start_parameter));
  }

  if (object.has_field("web_app")) {
    TRY_RESULT(web_app, object.extract_required_field("web_app", td::JsonValue::Type::Object));
    auto &web_app_object = web_app.get_object();
    TRY_RESULT(url, web_app_object.get_required_string_field("url"));
    return make_object<td_api::inlineQueryResultsButton>(text,
                                                         make_object<td_api::inlineQueryResultsButtonTypeWebApp>(url));
  }

  return td::Status::Error(400, "InlineQueryResultsButton must have exactly one optional field");
}

td::Result<td_api::object_ptr<td_api::inlineQueryResultsButton>> Client::get_inline_query_results_button(
    td::MutableSlice button) {
  if (button.empty()) {
    return nullptr;
  }

  LOG(INFO) << "Parsing JSON object: " << button;
  auto r_value = json_decode(button);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse inline query results button JSON object");
  }

  auto r_button = get_inline_query_results_button(r_value.move_as_ok());
  if (r_button.is_error()) {
    return td::Status::Error(400, PSLICE()
                                      << "Can't parse inline query results button: " << r_button.error().message());
  }
  return r_button.move_as_ok();
}

td::Result<td::vector<td_api::object_ptr<td_api::InputInlineQueryResult>>> Client::get_inline_query_results(
    const Query *query, BotUserIds &bot_user_ids) const {
  auto results_encoded = query->arg("results");
  if (results_encoded.empty()) {
    return td::vector<object_ptr<td_api::InputInlineQueryResult>>();
  }

  LOG(INFO) << "Parsing JSON object: " << results_encoded;
  auto r_values = json_decode(results_encoded);
  if (r_values.is_error()) {
    return td::Status::Error(
        400, PSLICE() << "Can't parse JSON encoded inline query results: " << r_values.error().message());
  }

  return get_array_generic<td_api::InputInlineQueryResult>(
      r_values.move_as_ok(), "InlineQueryResult",
      [&](td::JsonValue &&value) { return get_inline_query_result(query, std::move(value), bot_user_ids); });
}

td::Result<td_api::object_ptr<td_api::InputInlineQueryResult>> Client::get_inline_query_result(
    const Query *query, BotUserIds &bot_user_ids) const {
  auto result_encoded = query->arg("result");
  if (result_encoded.empty()) {
    return td::Status::Error(400, "Result isn't specified");
  }

  LOG(INFO) << "Parsing JSON object: " << result_encoded;
  auto r_value = json_decode(result_encoded);
  if (r_value.is_error()) {
    return td::Status::Error(
        400, PSLICE() << "Can't parse JSON encoded web view query results " << r_value.error().message());
  }

  return get_inline_query_result(query, r_value.move_as_ok(), bot_user_ids);
}

td::Result<td_api::object_ptr<td_api::InputInlineQueryResult>> Client::get_inline_query_result(
    const Query *query, td::JsonValue &&value, BotUserIds &bot_user_ids) const {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Inline query result must be an object");
  }

  auto &object = value.get_object();

  TRY_RESULT(type, object.get_required_string_field("type"));
  td::to_lower_inplace(type);

  TRY_RESULT(id, object.get_required_string_field("id"));

  bool is_input_message_content_required = (type == "article");
  object_ptr<td_api::InputMessageContent> input_message_content;

  TRY_RESULT(input_message_content_obj,
             object.extract_optional_field("input_message_content", td::JsonValue::Type::Object));
  if (input_message_content_obj.type() == td::JsonValue::Type::Null) {
    // legacy
    TRY_RESULT(message_text, is_input_message_content_required ? object.get_required_string_field("message_text")
                                                               : object.get_optional_string_field("message_text"));
    TRY_RESULT(disable_web_page_preview, object.get_optional_bool_field("disable_web_page_preview"));
    TRY_RESULT(parse_mode, object.get_optional_string_field("parse_mode"));
    auto entities = object.extract_field("entities");

    if (is_input_message_content_required || !message_text.empty()) {
      TRY_RESULT(input_message_text,
                 get_input_message_text(std::move(message_text), get_link_preview_options(disable_web_page_preview),
                                        std::move(parse_mode), std::move(entities)));
      input_message_content = std::move(input_message_text);
    }
  } else {
    auto &input_message_content_object = input_message_content_obj.get_object();
    TRY_RESULT(input_message_content_result,
               get_input_message_content(query, input_message_content_object, is_input_message_content_required));
    input_message_content = std::move(input_message_content_result);
  }
  TRY_RESULT(input_caption, object.get_optional_string_field("caption"));
  TRY_RESULT(parse_mode, object.get_optional_string_field("parse_mode"));
  auto entities = object.extract_field("caption_entities");
  TRY_RESULT(caption, get_formatted_text(std::move(input_caption), std::move(parse_mode), std::move(entities)));
  TRY_RESULT(show_caption_above_media, object.get_optional_bool_field("show_caption_above_media"));

  TRY_RESULT(reply_markup_object, object.extract_optional_field("reply_markup", td::JsonValue::Type::Object));
  object_ptr<td_api::ReplyMarkup> reply_markup;
  if (reply_markup_object.type() != td::JsonValue::Type::Null) {
    TRY_RESULT_ASSIGN(reply_markup, get_reply_markup(std::move(reply_markup_object), bot_user_ids));
  }

  auto thumbnail_url_field_name = td::Slice("thumbnail_url");
  auto thumbnail_width_field_name = td::Slice("thumbnail_width");
  auto thumbnail_height_field_name = td::Slice("thumbnail_height");
  if (!object.has_field(thumbnail_url_field_name) && !object.has_field(thumbnail_width_field_name) &&
      !object.has_field(thumbnail_height_field_name)) {
    thumbnail_url_field_name = td::Slice("thumb_url");
    thumbnail_width_field_name = td::Slice("thumb_width");
    thumbnail_height_field_name = td::Slice("thumb_height");
  }
  TRY_RESULT(thumbnail_url, object.get_optional_string_field(thumbnail_url_field_name));
  TRY_RESULT(thumbnail_width, object.get_optional_int_field(thumbnail_width_field_name));
  TRY_RESULT(thumbnail_height, object.get_optional_int_field(thumbnail_height_field_name));

  object_ptr<td_api::InputInlineQueryResult> result;
  if (type == "article") {
    TRY_RESULT(url, object.get_optional_string_field("url"));
    TRY_RESULT(hide_url, object.get_optional_bool_field("hide_url"));
    TRY_RESULT(title, object.get_required_string_field("title"));
    TRY_RESULT(description, object.get_optional_string_field("description"));

    CHECK(input_message_content != nullptr);
    return make_object<td_api::inputInlineQueryResultArticle>(
        id, hide_url ? td::string() : url, title, description, thumbnail_url, thumbnail_width, thumbnail_height,
        std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "audio") {
    TRY_RESULT(audio_url, object.get_optional_string_field("audio_url"));
    TRY_RESULT(audio_duration, object.get_optional_int_field("audio_duration"));
    TRY_RESULT(title, audio_url.empty() ? object.get_optional_string_field("title")
                                        : object.get_required_string_field("title"));
    TRY_RESULT(performer, object.get_optional_string_field("performer"));
    if (audio_url.empty()) {
      TRY_RESULT_ASSIGN(audio_url, object.get_required_string_field("audio_file_id"));
    }

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageAudio>(nullptr, std::move(caption));
    }
    return make_object<td_api::inputInlineQueryResultAudio>(id, title, performer, audio_url, audio_duration,
                                                            std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "contact") {
    TRY_RESULT(phone_number, object.get_required_string_field("phone_number"));
    TRY_RESULT(first_name, object.get_required_string_field("first_name"));
    TRY_RESULT(last_name, object.get_optional_string_field("last_name"));
    TRY_RESULT(vcard, object.get_optional_string_field("vcard"));

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageContact>(
          make_object<td_api::contact>(phone_number, first_name, last_name, vcard, 0));
    }
    return make_object<td_api::inputInlineQueryResultContact>(
        id, make_object<td_api::contact>(phone_number, first_name, last_name, vcard, 0), thumbnail_url, thumbnail_width,
        thumbnail_height, std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "document") {
    TRY_RESULT(title, object.get_required_string_field("title"));
    TRY_RESULT(description, object.get_optional_string_field("description"));
    TRY_RESULT(document_url, object.get_optional_string_field("document_url"));
    TRY_RESULT(mime_type, document_url.empty() ? object.get_optional_string_field("mime_type")
                                               : object.get_required_string_field("mime_type"));
    if (document_url.empty()) {
      TRY_RESULT_ASSIGN(document_url, object.get_required_string_field("document_file_id"));
    }

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageDocument>(nullptr, std::move(caption));
    }
    return make_object<td_api::inputInlineQueryResultDocument>(
        id, title, description, document_url, mime_type, thumbnail_url, thumbnail_width, thumbnail_height,
        std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "game") {
    TRY_RESULT(game_short_name, object.get_required_string_field("game_short_name"));
    return make_object<td_api::inputInlineQueryResultGame>(id, game_short_name, std::move(reply_markup));
  }
  if (type == "gif") {
    TRY_RESULT(title, object.get_optional_string_field("title"));
    TRY_RESULT(gif_url, object.get_optional_string_field("gif_url"));
    auto thumbnail_mime_type_field_name = td::Slice("thumbnail_mime_type");
    if (!object.has_field(thumbnail_mime_type_field_name)) {
      thumbnail_mime_type_field_name = td::Slice("thumb_mime_type");
    }
    TRY_RESULT(thumbnail_mime_type, object.get_optional_string_field(thumbnail_mime_type_field_name));
    TRY_RESULT(gif_duration, object.get_optional_int_field("gif_duration"));
    TRY_RESULT(gif_width, object.get_optional_int_field("gif_width"));
    TRY_RESULT(gif_height, object.get_optional_int_field("gif_height"));
    if (gif_url.empty()) {
      TRY_RESULT_ASSIGN(gif_url, object.get_required_string_field("gif_file_id"));
    }

    if (input_message_content == nullptr) {
      input_message_content =
          make_object<td_api::inputMessageAnimation>(nullptr, std::move(caption), show_caption_above_media, false);
    }
    return make_object<td_api::inputInlineQueryResultAnimation>(
        id, title, thumbnail_url, thumbnail_mime_type, gif_url, "image/gif", gif_duration, gif_width, gif_height,
        std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "location") {
    if (input_message_content == nullptr) {
      TRY_RESULT(location, get_location(object));
      TRY_RESULT(live_period, object.get_optional_int_field("live_period"));
      TRY_RESULT(heading, object.get_optional_int_field("heading"));
      TRY_RESULT(proximity_alert_radius, object.get_optional_int_field("proximity_alert_radius"));
      if (live_period != 0) {
        input_message_content = make_object<td_api::inputMessageLiveLocation>(
            make_object<td_api::liveLocation>(std::move(location), live_period, heading, proximity_alert_radius));
      } else {
        input_message_content = make_object<td_api::inputMessageLocation>(std::move(location));
      }
    }

    TRY_RESULT(location, get_location(object));
    TRY_RESULT(live_period, object.get_optional_int_field("live_period"));
    TRY_RESULT(title, object.get_required_string_field("title"));
    return make_object<td_api::inputInlineQueryResultLocation>(
        id, std::move(location), live_period, title, thumbnail_url, thumbnail_width, thumbnail_height,
        std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "mpeg4_gif") {
    TRY_RESULT(title, object.get_optional_string_field("title"));
    TRY_RESULT(mpeg4_url, object.get_optional_string_field("mpeg4_url"));
    auto thumbnail_mime_type_field_name = td::Slice("thumbnail_mime_type");
    if (!object.has_field(thumbnail_mime_type_field_name)) {
      thumbnail_mime_type_field_name = td::Slice("thumb_mime_type");
    }
    TRY_RESULT(thumbnail_mime_type, object.get_optional_string_field(thumbnail_mime_type_field_name));
    TRY_RESULT(mpeg4_duration, object.get_optional_int_field("mpeg4_duration"));
    TRY_RESULT(mpeg4_width, object.get_optional_int_field("mpeg4_width"));
    TRY_RESULT(mpeg4_height, object.get_optional_int_field("mpeg4_height"));
    if (mpeg4_url.empty()) {
      TRY_RESULT_ASSIGN(mpeg4_url, object.get_required_string_field("mpeg4_file_id"));
    }

    if (input_message_content == nullptr) {
      input_message_content =
          make_object<td_api::inputMessageAnimation>(nullptr, std::move(caption), show_caption_above_media, false);
    }
    return make_object<td_api::inputInlineQueryResultAnimation>(
        id, title, thumbnail_url, thumbnail_mime_type, mpeg4_url, "video/mp4", mpeg4_duration, mpeg4_width,
        mpeg4_height, std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "photo") {
    TRY_RESULT(title, object.get_optional_string_field("title"));
    TRY_RESULT(description, object.get_optional_string_field("description"));
    TRY_RESULT(photo_url, object.get_optional_string_field("photo_url"));
    TRY_RESULT(photo_width, object.get_optional_int_field("photo_width"));
    TRY_RESULT(photo_height, object.get_optional_int_field("photo_height"));
    if (photo_url.empty()) {
      TRY_RESULT_ASSIGN(photo_url, object.get_required_string_field("photo_file_id"));
    }

    if (input_message_content == nullptr) {
      input_message_content =
          make_object<td_api::inputMessagePhoto>(nullptr, std::move(caption), show_caption_above_media, nullptr, false);
    }
    return make_object<td_api::inputInlineQueryResultPhoto>(id, title, description, thumbnail_url, photo_url,
                                                            photo_width, photo_height, std::move(reply_markup),
                                                            std::move(input_message_content));
  }
  if (type == "sticker") {
    TRY_RESULT(sticker_file_id, object.get_required_string_field("sticker_file_id"));

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageSticker>(nullptr, td::string());
    }
    return make_object<td_api::inputInlineQueryResultSticker>(id, "", sticker_file_id, 0, 0, std::move(reply_markup),
                                                              std::move(input_message_content));
  }
  if (type == "venue") {
    if (input_message_content == nullptr) {
      TRY_RESULT(venue, get_venue(object));
      input_message_content = make_object<td_api::inputMessageVenue>(std::move(venue));
    }

    TRY_RESULT(venue, get_venue(object));
    return make_object<td_api::inputInlineQueryResultVenue>(id, std::move(venue), thumbnail_url, thumbnail_width,
                                                            thumbnail_height, std::move(reply_markup),
                                                            std::move(input_message_content));
  }
  if (type == "video") {
    TRY_RESULT(title, object.get_required_string_field("title"));
    TRY_RESULT(description, object.get_optional_string_field("description"));
    TRY_RESULT(video_url, object.get_optional_string_field("video_url"));
    TRY_RESULT(mime_type, video_url.empty() ? object.get_optional_string_field("mime_type")
                                            : object.get_required_string_field("mime_type"));
    TRY_RESULT(video_width, object.get_optional_int_field("video_width"));
    TRY_RESULT(video_height, object.get_optional_int_field("video_height"));
    TRY_RESULT(video_duration, object.get_optional_int_field("video_duration"));
    // TRY_RESULT(video_start_timestamp, object.get_optional_int_field("video_start_timestamp"));
    if (video_url.empty()) {
      TRY_RESULT_ASSIGN(video_url, object.get_required_string_field("video_file_id"));
    }

    if (input_message_content == nullptr) {
      input_message_content =
          make_object<td_api::inputMessageVideo>(nullptr, std::move(caption), show_caption_above_media, nullptr, false);
    }
    return make_object<td_api::inputInlineQueryResultVideo>(id, title, description, thumbnail_url, video_url, mime_type,
                                                            video_width, video_height, video_duration,
                                                            std::move(reply_markup), std::move(input_message_content));
  }
  if (type == "voice") {
    TRY_RESULT(title, object.get_required_string_field("title"));
    TRY_RESULT(voice_note_url, object.get_optional_string_field("voice_url"));
    TRY_RESULT(voice_note_duration, object.get_optional_int_field("voice_duration"));
    if (voice_note_url.empty()) {
      TRY_RESULT_ASSIGN(voice_note_url, object.get_required_string_field("voice_file_id"));
    }

    if (input_message_content == nullptr) {
      input_message_content = make_object<td_api::inputMessageVoiceNote>(nullptr, std::move(caption), nullptr);
    }
    return make_object<td_api::inputInlineQueryResultVoiceNote>(
        id, title, voice_note_url, voice_note_duration, std::move(reply_markup), std::move(input_message_content));
  }

  return td::Status::Error(400, PSLICE() << "type \"" << type << "\" is unsupported for the inline query result");
}

td::Result<Client::BotCommandScope> Client::get_bot_command_scope(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "BotCommandScope must be an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(type, object.get_required_string_field("type"));
  if (type == "default") {
    return BotCommandScope(make_object<td_api::botCommandScopeDefault>());
  }
  if (type == "all_private_chats") {
    return BotCommandScope(make_object<td_api::botCommandScopeAllPrivateChats>());
  }
  if (type == "all_group_chats") {
    return BotCommandScope(make_object<td_api::botCommandScopeAllGroupChats>());
  }
  if (type == "all_chat_administrators") {
    return BotCommandScope(make_object<td_api::botCommandScopeAllChatAdministrators>());
  }
  if (type != "chat" && type != "chat_administrators" && type != "chat_member") {
    return td::Status::Error(400, "Unsupported type specified");
  }

  TRY_RESULT(chat_id, object.get_required_string_field("chat_id"));
  if (chat_id.empty()) {
    return td::Status::Error(400, "Empty chat_id specified");
  }
  if (type == "chat") {
    return BotCommandScope(make_object<td_api::botCommandScopeChat>(0), std::move(chat_id));
  }
  if (type == "chat_administrators") {
    return BotCommandScope(make_object<td_api::botCommandScopeChatAdministrators>(0), std::move(chat_id));
  }

  TRY_RESULT(user_id, object.get_required_long_field("user_id"));
  if (user_id <= 0) {
    return td::Status::Error(400, "Invalid user_id specified");
  }
  CHECK(type == "chat_member");
  return BotCommandScope(make_object<td_api::botCommandScopeChatMember>(0, user_id), std::move(chat_id), user_id);
}

td::Result<Client::BotCommandScope> Client::get_bot_command_scope(const Query *query) {
  auto scope = query->arg("scope");
  if (scope.empty()) {
    return BotCommandScope(make_object<td_api::botCommandScopeDefault>());
  }

  LOG(INFO) << "Parsing JSON object: " << scope;
  auto r_value = json_decode(scope);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse BotCommandScope JSON object");
  }

  auto r_scope = get_bot_command_scope(r_value.move_as_ok());
  if (r_scope.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse BotCommandScope: " << r_scope.error().message());
  }
  return r_scope.move_as_ok();
}

td::Result<td_api::object_ptr<td_api::botCommand>> Client::get_bot_command(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "expected an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(command, object.get_required_string_field("command"));
  TRY_RESULT(description, object.get_required_string_field("description"));
  TRY_RESULT(is_ephemeral, object.get_optional_bool_field("is_ephemeral"));

  return make_object<td_api::botCommand>(command, description, is_ephemeral);
}

td::Result<td::vector<td_api::object_ptr<td_api::botCommand>>> Client::get_bot_commands(const Query *query) {
  auto commands = query->arg("commands");
  if (commands.empty()) {
    return td::vector<object_ptr<td_api::botCommand>>();
  }
  LOG(INFO) << "Parsing JSON object: " << commands;
  auto r_value = json_decode(commands);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse commands JSON object");
  }

  return get_array(r_value.move_as_ok(), "BotCommand", get_bot_command);
}

td::Result<td_api::object_ptr<td_api::botMenuButton>> Client::get_bot_menu_button(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "MenuButton must be an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(type, object.get_required_string_field("type"));
  if (type == "default") {
    return make_object<td_api::botMenuButton>("", "default");
  }
  if (type == "commands") {
    return nullptr;
  }
  if (type == "web_app") {
    TRY_RESULT(text, object.get_required_string_field("text"));
    TRY_RESULT(web_app, object.extract_required_field("web_app", td::JsonValue::Type::Object));
    auto &web_app_object = web_app.get_object();
    TRY_RESULT(url, web_app_object.get_required_string_field("url"));
    return make_object<td_api::botMenuButton>(text, url);
  }

  return td::Status::Error(400, "MenuButton has unsupported type");
}

td::Result<td_api::object_ptr<td_api::botMenuButton>> Client::get_bot_menu_button(const Query *query) {
  auto menu_button = query->arg("menu_button");
  if (menu_button.empty()) {
    return make_object<td_api::botMenuButton>("", "default");
  }

  LOG(INFO) << "Parsing JSON object: " << menu_button;
  auto r_value = json_decode(menu_button);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse menu button JSON object");
  }

  auto r_menu_button = get_bot_menu_button(r_value.move_as_ok());
  if (r_menu_button.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse menu button: " << r_menu_button.error().message());
  }
  return r_menu_button.move_as_ok();
}

td::Result<td_api::object_ptr<td_api::chatAdministratorRights>> Client::get_chat_administrator_rights(
    td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "ChatAdministratorRights must be an Object");
  }

  auto &object = value.get_object();
  TRY_RESULT(can_manage_chat, object.get_optional_bool_field("can_manage_chat"));
  TRY_RESULT(can_change_info, object.get_optional_bool_field("can_change_info"));
  TRY_RESULT(can_post_messages, object.get_optional_bool_field("can_post_messages"));
  TRY_RESULT(can_edit_messages, object.get_optional_bool_field("can_edit_messages"));
  TRY_RESULT(can_delete_messages, object.get_optional_bool_field("can_delete_messages"));
  TRY_RESULT(can_invite_users, object.get_optional_bool_field("can_invite_users"));
  TRY_RESULT(can_restrict_members, object.get_optional_bool_field("can_restrict_members"));
  TRY_RESULT(can_pin_messages, object.get_optional_bool_field("can_pin_messages"));
  TRY_RESULT(can_manage_topics, object.get_optional_bool_field("can_manage_topics"));
  TRY_RESULT(can_promote_members, object.get_optional_bool_field("can_promote_members"));
  TRY_RESULT(can_manage_video_chats, object.get_optional_bool_field("can_manage_video_chats"));
  TRY_RESULT(can_post_stories, object.get_optional_bool_field("can_post_stories"));
  TRY_RESULT(can_edit_stories, object.get_optional_bool_field("can_edit_stories"));
  TRY_RESULT(can_delete_stories, object.get_optional_bool_field("can_delete_stories"));
  TRY_RESULT(can_manage_direct_messages, object.get_optional_bool_field("can_manage_direct_messages"));
  TRY_RESULT(can_manage_tags, object.get_optional_bool_field("can_manage_tags"));
  TRY_RESULT(can_send_welcome_messages, object.get_optional_bool_field("can_send_welcome_messages"));
  TRY_RESULT(is_anonymous, object.get_optional_bool_field("is_anonymous"));
  return make_object<td_api::chatAdministratorRights>(
      can_manage_chat, can_change_info, can_post_messages, can_edit_messages, can_delete_messages, can_invite_users,
      can_restrict_members, can_pin_messages, can_manage_topics, can_promote_members, can_manage_video_chats,
      can_post_stories, can_edit_stories, can_delete_stories, can_manage_direct_messages, can_manage_tags,
      can_send_welcome_messages, is_anonymous);
}

td::Result<td_api::object_ptr<td_api::chatAdministratorRights>> Client::get_chat_administrator_rights(
    const Query *query) {
  auto rights = query->arg("rights");
  if (rights.empty()) {
    return nullptr;
  }

  LOG(INFO) << "Parsing JSON object: " << rights;
  auto r_value = json_decode(rights);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse ChatAdministratorRights JSON object");
  }

  auto r_rights = get_chat_administrator_rights(r_value.move_as_ok());
  if (r_rights.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse ChatAdministratorRights: " << r_rights.error().message());
  }
  return r_rights.move_as_ok();
}

td::Result<td_api::object_ptr<td_api::maskPosition>> Client::get_mask_position(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    if (value.type() == td::JsonValue::Type::Null) {
      return nullptr;
    }
    return td::Status::Error(400, "MaskPosition must be an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(point_str, object.get_required_string_field("point"));
  point_str = td::trim(td::to_lower(point_str));
  int32 point;
  for (point = 0; point < MASK_POINTS_SIZE; point++) {
    if (MASK_POINTS[point] == point_str) {
      break;
    }
  }
  if (point == MASK_POINTS_SIZE) {
    return td::Status::Error(400, "Wrong point specified in MaskPosition");
  }

  TRY_RESULT(x_shift, object.get_required_double_field("x_shift"));
  TRY_RESULT(y_shift, object.get_required_double_field("y_shift"));
  TRY_RESULT(scale, object.get_required_double_field("scale"));

  return make_object<td_api::maskPosition>(mask_index_to_point(point), x_shift, y_shift, scale);
}

td::int32 Client::mask_point_to_index(const object_ptr<td_api::MaskPoint> &mask_point) {
  CHECK(mask_point != nullptr);
  switch (mask_point->get_id()) {
    case td_api::maskPointForehead::ID:
      return 0;
    case td_api::maskPointEyes::ID:
      return 1;
    case td_api::maskPointMouth::ID:
      return 2;
    case td_api::maskPointChin::ID:
      return 3;
    default:
      UNREACHABLE();
      return -1;
  }
}

td_api::object_ptr<td_api::MaskPoint> Client::mask_index_to_point(int32 index) {
  switch (index) {
    case 0:
      return make_object<td_api::maskPointForehead>();
    case 1:
      return make_object<td_api::maskPointEyes>();
    case 2:
      return make_object<td_api::maskPointMouth>();
    case 3:
      return make_object<td_api::maskPointChin>();
    default:
      UNREACHABLE();
      return nullptr;
  }
}

td::Result<td_api::object_ptr<td_api::maskPosition>> Client::get_mask_position(const Query *query,
                                                                               td::Slice field_name) {
  auto mask_position = query->arg(field_name);
  if (mask_position.empty()) {
    return nullptr;
  }

  LOG(INFO) << "Parsing JSON object: " << mask_position;
  auto r_value = json_decode(mask_position);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse mask position JSON object");
  }

  auto r_mask_position = get_mask_position(r_value.move_as_ok());
  if (r_mask_position.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse mask position: " << r_mask_position.error().message());
  }
  return r_mask_position.move_as_ok();
}

td::Result<td::string> Client::get_sticker_emojis(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Array) {
    return td::Status::Error(400, "expected an Array of string");
  }

  td::string result;
  auto emoji_count = value.get_array().size();
  if (emoji_count == 0) {
    return td::Status::Error(400, "emoji list must be non-empty");
  }
  if (emoji_count > MAX_STICKER_EMOJI_COUNT) {
    return td::Status::Error(400, "too many emoji specified");
  }
  for (auto &emoji : value.get_array()) {
    if (emoji.type() != td::JsonValue::Type::String) {
      return td::Status::Error(400, "emoji must be a string");
    }
    if (!td::is_emoji(emoji.get_string())) {
      return td::Status::Error(400, "expected a Unicode emoji");
    }
    result += emoji.get_string().str();
  }
  return std::move(result);
}

td::Result<td::string> Client::get_sticker_emojis(td::MutableSlice emoji_list) {
  LOG(INFO) << "Parsing JSON object: " << emoji_list;
  auto r_value = json_decode(emoji_list);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse emoji list JSON array");
  }

  auto r_emojis = get_sticker_emojis(r_value.move_as_ok());
  if (r_emojis.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse emoji list: " << r_emojis.error().message());
  }
  return r_emojis.move_as_ok();
}

td::Result<td_api::object_ptr<td_api::StickerFormat>> Client::get_sticker_format(td::Slice sticker_format) {
  if (sticker_format == "static") {
    return make_object<td_api::stickerFormatWebp>();
  }
  if (sticker_format == "animated") {
    return make_object<td_api::stickerFormatTgs>();
  }
  if (sticker_format == "video") {
    return make_object<td_api::stickerFormatWebm>();
  }
  if (sticker_format == "auto") {
    return nullptr;
  }
  return td::Status::Error(400, "Invalid sticker format specified");
}

td::Result<td_api::object_ptr<td_api::newSticker>> Client::get_new_sticker(const Query *query, td::JsonValue &&value,
                                                                           td::Slice default_sticker_format) const {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "InputSticker must be an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(sticker, object.get_optional_string_field("sticker"));
  auto input_file = get_input_file(query, td::Slice(), sticker, false);
  if (input_file == nullptr) {
    return td::Status::Error(400, "sticker not found");
  }
  td::string sticker_format_str;
  if (default_sticker_format.empty()) {
    TRY_RESULT_ASSIGN(sticker_format_str, object.get_required_string_field("format"));
  } else {
    TRY_RESULT_ASSIGN(sticker_format_str, object.get_optional_string_field("format"));
    if (sticker_format_str.empty()) {
      sticker_format_str = default_sticker_format.str();
    }
  }
  TRY_RESULT(sticker_format, get_sticker_format(sticker_format_str));
  TRY_RESULT(emoji_list, object.extract_required_field("emoji_list", td::JsonValue::Type::Array));
  TRY_RESULT(emojis, get_sticker_emojis(std::move(emoji_list)));
  TRY_RESULT(mask_position, get_mask_position(object.extract_field("mask_position")));
  td::vector<td::string> input_keywords;
  if (object.has_field("keywords")) {
    TRY_RESULT(keywords, object.extract_required_field("keywords", td::JsonValue::Type::Array));
    for (auto &keyword : keywords.get_array()) {
      if (keyword.type() != td::JsonValue::Type::String) {
        return td::Status::Error(400, "keyword must be a string");
      }
      input_keywords.push_back(keyword.get_string().str());
    }
  }
  return make_object<td_api::newSticker>(std::move(input_file), std::move(sticker_format), emojis,
                                         std::move(mask_position), std::move(input_keywords));
}

td::Result<td_api::object_ptr<td_api::newSticker>> Client::get_new_sticker(const Query *query) const {
  if (query->has_arg("sticker") || query->file("sticker") != nullptr) {
    auto sticker = query->arg("sticker");
    LOG(INFO) << "Parsing JSON object: " << sticker;
    auto r_value = json_decode(sticker);
    if (r_value.is_error()) {
      LOG(INFO) << "Can't parse JSON object: " << r_value.error();
      return td::Status::Error(400, "Can't parse sticker JSON object");
    }

    auto r_sticker = get_new_sticker(query, r_value.move_as_ok(), "auto");
    if (r_sticker.is_error()) {
      return td::Status::Error(400, PSLICE() << "Can't parse sticker: " << r_sticker.error().message());
    }
    return r_sticker.move_as_ok();
  }

  return get_legacy_new_sticker(query);
}

td::Result<td_api::object_ptr<td_api::newSticker>> Client::get_legacy_new_sticker(const Query *query) const {
  auto emojis = query->arg("emojis");
  auto sticker = get_input_file(query, "png_sticker");
  object_ptr<td_api::StickerFormat> sticker_format;
  object_ptr<td_api::maskPosition> mask_position;
  if (sticker != nullptr) {
    sticker_format = make_object<td_api::stickerFormatWebp>();
    TRY_RESULT_ASSIGN(mask_position, get_mask_position(query, "mask_position"));
  } else {
    sticker = get_input_file(query, "tgs_sticker", true);
    if (sticker != nullptr) {
      sticker_format = make_object<td_api::stickerFormatTgs>();
    } else {
      sticker = get_input_file(query, "webm_sticker", true);
      if (sticker != nullptr) {
        sticker_format = make_object<td_api::stickerFormatWebm>();
      } else {
        if (!query->arg("tgs_sticker").empty()) {
          return td::Status::Error(400, "Bad Request: animated sticker must be uploaded as an InputFile");
        }
        if (!query->arg("webm_sticker").empty()) {
          return td::Status::Error(400, "Bad Request: video sticker must be uploaded as an InputFile");
        }
        return td::Status::Error(400, "Bad Request: there is no sticker file in the request");
      }
    }
  }

  return make_object<td_api::newSticker>(std::move(sticker), std::move(sticker_format), emojis.str(),
                                         std::move(mask_position), td::vector<td::string>());
}

td::Result<td::vector<td_api::object_ptr<td_api::newSticker>>> Client::get_new_stickers(const Query *query) const {
  if (query->has_arg("stickers")) {
    auto sticker_format_str = query->arg("sticker_format");
    auto stickers = query->arg("stickers");
    LOG(INFO) << "Parsing JSON object: " << stickers;
    auto r_value = json_decode(stickers);
    if (r_value.is_error()) {
      LOG(INFO) << "Can't parse JSON object: " << r_value.error();
      return td::Status::Error(400, "Can't parse stickers JSON object");
    }

    return get_array_generic<td_api::newSticker>(r_value.move_as_ok(), "InputSticker", [&](td::JsonValue &&value) {
      return get_new_sticker(query, std::move(value), sticker_format_str);
    });
  }

  TRY_RESULT(new_sticker, get_legacy_new_sticker(query));

  td::vector<object_ptr<td_api::newSticker>> stickers;
  stickers.push_back(std::move(new_sticker));
  return std::move(stickers);
}

td::Result<td_api::object_ptr<td_api::InputFile>> Client::get_sticker_input_file(const Query *query,
                                                                                 td::Slice field_name) {
  auto file_id = trim(query->arg(field_name));
  if (file_id.empty()) {
    return td::Status::Error(400, "Sticker is not specified");
  }
  return make_object<td_api::inputFileRemote>(file_id.str());
}

td::Result<td::string> Client::get_passport_element_hash(td::Slice encoded_hash) {
  if (!td::is_base64(encoded_hash)) {
    return td::Status::Error(400, "hash isn't a valid base64-encoded string");
  }
  return td::base64_decode(encoded_hash).move_as_ok();
}

td::Result<td_api::object_ptr<td_api::InputPassportElementErrorSource>> Client::get_passport_element_error_source(
    td::JsonObject &object) {
  TRY_RESULT(source, object.get_optional_string_field("source"));

  if (source.empty() || source == "unspecified") {
    TRY_RESULT(element_hash, object.get_required_string_field("element_hash"));
    TRY_RESULT(hash, get_passport_element_hash(element_hash));

    return make_object<td_api::inputPassportElementErrorSourceUnspecified>(hash);
  }
  if (source == "data") {
    TRY_RESULT(data_hash, object.get_required_string_field("data_hash"));
    TRY_RESULT(hash, get_passport_element_hash(data_hash));

    TRY_RESULT(field_name, object.get_required_string_field("field_name"));
    return make_object<td_api::inputPassportElementErrorSourceDataField>(field_name, hash);
  }
  if (source == "file" || source == "selfie" || source == "translation_file" || source == "front_side" ||
      source == "reverse_side") {
    TRY_RESULT(file_hash, object.get_required_string_field("file_hash"));
    TRY_RESULT(hash, get_passport_element_hash(file_hash));

    if (source == "front_side") {
      return make_object<td_api::inputPassportElementErrorSourceFrontSide>(hash);
    }
    if (source == "reverse_side") {
      return make_object<td_api::inputPassportElementErrorSourceReverseSide>(hash);
    }
    if (source == "selfie") {
      return make_object<td_api::inputPassportElementErrorSourceSelfie>(hash);
    }
    if (source == "translation_file") {
      return make_object<td_api::inputPassportElementErrorSourceTranslationFile>(hash);
    }
    if (source == "file") {
      return make_object<td_api::inputPassportElementErrorSourceFile>(hash);
    }
    UNREACHABLE();
  }
  if (source == "files" || source == "translation_files") {
    td::vector<td::string> input_hashes;
    TRY_RESULT(file_hashes, object.extract_required_field("file_hashes", td::JsonValue::Type::Array));
    for (auto &input_hash : file_hashes.get_array()) {
      if (input_hash.type() != td::JsonValue::Type::String) {
        return td::Status::Error(400, "hash must be a string");
      }
      TRY_RESULT(hash, get_passport_element_hash(input_hash.get_string()));
      input_hashes.push_back(std::move(hash));
    }
    if (source == "files") {
      return make_object<td_api::inputPassportElementErrorSourceFiles>(std::move(input_hashes));
    }
    if (source == "translation_files") {
      return make_object<td_api::inputPassportElementErrorSourceTranslationFiles>(std::move(input_hashes));
    }
    UNREACHABLE();
  }
  return td::Status::Error(400, "wrong source specified");
}

td::Result<td_api::object_ptr<td_api::inputPassportElementError>> Client::get_passport_element_error(
    td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "expected an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(input_type, object.get_required_string_field("type"));
  auto type = get_passport_element_type(input_type);
  if (type == nullptr) {
    return td::Status::Error(400, "wrong Telegram Passport element type specified");
  }
  TRY_RESULT(message, object.get_required_string_field("message"));
  TRY_RESULT(source, get_passport_element_error_source(object));

  return make_object<td_api::inputPassportElementError>(std::move(type), message, std::move(source));
}

td::Result<td::vector<td_api::object_ptr<td_api::inputPassportElementError>>> Client::get_passport_element_errors(
    const Query *query) {
  auto input_errors = query->arg("errors");
  LOG(INFO) << "Parsing JSON object: " << input_errors;
  auto r_value = json_decode(input_errors);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse errors JSON object");
  }

  return get_array(r_value.move_as_ok(), "PassportElementError", get_passport_element_error);
}

td::JsonValue Client::get_input_entities(const Query *query, td::Slice field_name) {
  auto entities = query->arg(field_name);
  if (!entities.empty()) {
    auto r_value = json_decode(entities);
    if (r_value.is_ok()) {
      return r_value.move_as_ok();
    }

    LOG(INFO) << "Can't parse entities JSON object: " << r_value.error();
  }

  return td::JsonValue();
}

td::Result<td_api::object_ptr<td_api::formattedText>> Client::get_caption(const Query *query) {
  return get_formatted_text(query->arg("caption").str(), query->arg("parse_mode").str(),
                            get_input_entities(query, "caption_entities"));
}

td::Result<td_api::object_ptr<td_api::TextEntityType>> Client::get_text_entity_type(td::JsonObject &object) {
  TRY_RESULT(type, object.get_required_string_field("type"));
  if (type.empty()) {
    return td::Status::Error("Type is not specified");
  }

  if (type == "bold") {
    return make_object<td_api::textEntityTypeBold>();
  }
  if (type == "italic") {
    return make_object<td_api::textEntityTypeItalic>();
  }
  if (type == "underline") {
    return make_object<td_api::textEntityTypeUnderline>();
  }
  if (type == "strikethrough") {
    return make_object<td_api::textEntityTypeStrikethrough>();
  }
  if (type == "spoiler") {
    return make_object<td_api::textEntityTypeSpoiler>();
  }
  if (type == "code") {
    return make_object<td_api::textEntityTypeCode>();
  }
  if (type == "pre") {
    TRY_RESULT(language, object.get_optional_string_field("language"));
    if (language.empty()) {
      return make_object<td_api::textEntityTypePre>();
    }
    return make_object<td_api::textEntityTypePreCode>(language);
  }
  if (type == "text_link") {
    TRY_RESULT(url, object.get_required_string_field("url"));
    return make_object<td_api::textEntityTypeTextUrl>(url);
  }
  if (type == "text_mention") {
    TRY_RESULT(user, object.extract_required_field("user", td::JsonValue::Type::Object));
    CHECK(user.type() == td::JsonValue::Type::Object);
    const auto &user_object = user.get_object();
    TRY_RESULT(user_id, user_object.get_required_long_field("id"));
    return make_object<td_api::textEntityTypeMentionName>(user_id);
  }
  if (type == "custom_emoji") {
    TRY_RESULT(custom_emoji_id, object.get_required_long_field("custom_emoji_id"));
    return make_object<td_api::textEntityTypeCustomEmoji>(custom_emoji_id);
  }
  if (type == "blockquote") {
    return make_object<td_api::textEntityTypeBlockQuote>();
  }
  if (type == "expandable_blockquote") {
    return make_object<td_api::textEntityTypeExpandableBlockQuote>();
  }
  if (type == "date_time") {
    TRY_RESULT(unix_time, object.get_required_int_field("unix_time"));
    TRY_RESULT(format, object.get_optional_string_field("date_time_format"));
    TRY_RESULT(formatting_type, get_date_time_formatting_type(format));
    return make_object<td_api::textEntityTypeDateTime>(unix_time, std::move(formatting_type));
  }
  if (type == "mention" || type == "hashtag" || type == "cashtag" || type == "bot_command" || type == "url" ||
      type == "email" || type == "phone_number" || type == "bank_card_number") {
    return nullptr;
  }

  return td::Status::Error("Unsupported type specified");
}

td::Result<td_api::object_ptr<td_api::textEntity>> Client::get_text_entity(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "expected an Object");
  }

  auto &object = value.get_object();
  TRY_RESULT(offset, object.get_required_int_field("offset"));
  TRY_RESULT(length, object.get_required_int_field("length"));
  TRY_RESULT(type, get_text_entity_type(object));

  if (type == nullptr) {
    return nullptr;
  }

  return make_object<td_api::textEntity>(offset, length, std::move(type));
}

td::Result<td_api::object_ptr<td_api::formattedText>> Client::get_formatted_text(td::string text, td::string parse_mode,
                                                                                 td::JsonValue &&input_entities) {
  if (text.size() > (1 << 15)) {
    return td::Status::Error(400, "Text is too long");
  }

  td::to_lower_inplace(parse_mode);
  if (!text.empty() && !parse_mode.empty() && parse_mode != "none") {
    object_ptr<td_api::TextParseMode> text_parse_mode;
    if (parse_mode == "markdown") {
      text_parse_mode = make_object<td_api::textParseModeMarkdown>(1);
    } else if (parse_mode == "markdownv2") {
      text_parse_mode = make_object<td_api::textParseModeMarkdown>(2);
    } else if (parse_mode == "html") {
      text_parse_mode = make_object<td_api::textParseModeHTML>();
    } else {
      return td::Status::Error(400, "Unsupported parse_mode");
    }

    auto parsed_text = execute(make_object<td_api::parseTextEntities>(text, std::move(text_parse_mode)));
    if (parsed_text->get_id() == td_api::error::ID) {
      auto error = move_object_as<td_api::error>(parsed_text);
      return td::Status::Error(error->code_, error->message_);
    }

    CHECK(parsed_text->get_id() == td_api::formattedText::ID);
    return move_object_as<td_api::formattedText>(parsed_text);
  }

  td::vector<object_ptr<td_api::textEntity>> entities;
  if (input_entities.type() == td::JsonValue::Type::Array) {
    for (auto &input_entity : input_entities.get_array()) {
      auto r_entity = get_text_entity(std::move(input_entity));
      if (r_entity.is_error()) {
        return td::Status::Error(400, PSLICE() << "Can't parse MessageEntity: " << r_entity.error().message());
      }
      if (r_entity.ok() == nullptr) {
        continue;
      }
      entities.push_back(r_entity.move_as_ok());
    }
  }

  return make_object<td_api::formattedText>(text, std::move(entities));
}

td_api::object_ptr<td_api::linkPreviewOptions> Client::get_link_preview_options(bool disable_web_page_preview) {
  // legacy
  if (!disable_web_page_preview) {
    return nullptr;
  }
  return make_object<td_api::linkPreviewOptions>(true, td::string(), false, false, false);
}

td::Result<td_api::object_ptr<td_api::linkPreviewOptions>> Client::get_link_preview_options(const Query *query) {
  auto link_preview_options = query->arg("link_preview_options");
  if (link_preview_options.empty()) {
    return get_link_preview_options(to_bool(query->arg("disable_web_page_preview")));
  }

  LOG(INFO) << "Parsing JSON object: " << link_preview_options;
  auto r_value = json_decode(link_preview_options);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse link preview options JSON object");
  }

  return get_link_preview_options(r_value.move_as_ok());
}

td::Result<td_api::object_ptr<td_api::linkPreviewOptions>> Client::get_link_preview_options(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Object expected as link preview options");
  }
  auto &object = value.get_object();
  TRY_RESULT(is_disabled, object.get_optional_bool_field("is_disabled"));
  TRY_RESULT(url, object.get_optional_string_field("url"));
  TRY_RESULT(prefer_small_media, object.get_optional_bool_field("prefer_small_media"));
  TRY_RESULT(prefer_large_media, object.get_optional_bool_field("prefer_large_media"));
  TRY_RESULT(show_above_text, object.get_optional_bool_field("show_above_text"));
  return make_object<td_api::linkPreviewOptions>(is_disabled, url, prefer_small_media, prefer_large_media,
                                                 show_above_text);
}

td::Result<td_api::object_ptr<td_api::inputMessageText>> Client::get_input_message_text(const Query *query) {
  TRY_RESULT(link_preview_options, get_link_preview_options(query));
  return get_input_message_text(query->arg("text").str(), std::move(link_preview_options),
                                query->arg("parse_mode").str(), get_input_entities(query, "entities"));
}

td::Result<td_api::object_ptr<td_api::inputMessageText>> Client::get_input_message_text(
    td::string text, object_ptr<td_api::linkPreviewOptions> link_preview_options, td::string parse_mode,
    td::JsonValue &&input_entities) {
  if (text.empty()) {
    return td::Status::Error(400, "Message text is empty");
  }

  TRY_RESULT(formatted_text, get_formatted_text(std::move(text), std::move(parse_mode), std::move(input_entities)));

  return make_object<td_api::inputMessageText>(std::move(formatted_text), std::move(link_preview_options), false);
}

td::Result<td_api::object_ptr<td_api::pageBlockCaption>> Client::get_page_block_caption(td::JsonValue &&value) {
  switch (value.type()) {
    case td::JsonValue::Type::Null:
      return nullptr;
    case td::JsonValue::Type::Object: {
      auto &object = value.get_object();
      TRY_RESULT(text, get_rich_text(object.extract_field("text")));
      TRY_RESULT(credit, get_rich_text(object.extract_field("credit")));
      return make_object<td_api::pageBlockCaption>(std::move(text), std::move(credit));
    }
    default:
      return td::Status::Error(400, "RichBlockCaption must be an object");
  }
}

td::Result<td_api::object_ptr<td_api::pageBlockTableCell>> Client::get_page_block_table_cell(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "RichBlockTableCell must be an object");
  }

  auto &object = value.get_object();
  TRY_RESULT(text, get_rich_text(object.extract_field("text")));
  TRY_RESULT(is_header, object.get_optional_bool_field("is_header"));
  TRY_RESULT(colspan, object.get_optional_int_field("colspan"));
  TRY_RESULT(rowspan, object.get_optional_int_field("rowspan"));

  object_ptr<td_api::PageBlockHorizontalAlignment> align;
  TRY_RESULT(align_str, object.get_optional_string_field("align"));
  if (align_str == "left" || (align_str.empty() && !is_header)) {
    align = make_object<td_api::pageBlockHorizontalAlignmentLeft>();
  } else if (align_str == "center" || (align_str.empty() && is_header)) {
    align = make_object<td_api::pageBlockHorizontalAlignmentCenter>();
  } else if (align_str == "right") {
    align = make_object<td_api::pageBlockHorizontalAlignmentRight>();
  } else {
    return td::Status::Error(400, "Invalid horizontal alignment specified");
  }

  object_ptr<td_api::PageBlockVerticalAlignment> valign;
  TRY_RESULT(valign_str, object.get_optional_string_field("valign"));
  if (valign_str == "top") {
    valign = make_object<td_api::pageBlockVerticalAlignmentTop>();
  } else if (valign_str == "middle" || valign_str.empty()) {
    valign = make_object<td_api::pageBlockVerticalAlignmentMiddle>();
  } else if (valign_str == "bottom") {
    valign = make_object<td_api::pageBlockVerticalAlignmentBottom>();
  } else {
    return td::Status::Error(400, "Invalid vertical alignment specified");
  }

  return make_object<td_api::pageBlockTableCell>(std::move(text), is_header, colspan, rowspan, std::move(align),
                                                 std::move(valign));
}

td::Result<td_api::object_ptr<td_api::RichText>> Client::get_rich_text(td::JsonValue &&value) {
  switch (value.type()) {
    case td::JsonValue::Type::Null:
      return nullptr;
    case td::JsonValue::Type::String:
      return make_object<td_api::richTextPlain>(value.get_string().str());
    case td::JsonValue::Type::Array: {
      td::vector<object_ptr<td_api::RichText>> texts;
      for (auto &input_text : value.get_array()) {
        TRY_RESULT(text, get_rich_text(std::move(input_text)));
        texts.push_back(std::move(text));
      }
      return make_object<td_api::richTexts>(std::move(texts));
    }
    case td::JsonValue::Type::Object: {
      auto &object = value.get_object();
      TRY_RESULT(type, object.get_required_string_field("type"));
      if (type == "bold") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextBold>(std::move(text));
      }
      if (type == "italic") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextItalic>(std::move(text));
      }
      if (type == "underline") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextUnderline>(std::move(text));
      }
      if (type == "strikethrough") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextStrikethrough>(std::move(text));
      }
      if (type == "spoiler") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextSpoiler>(std::move(text));
      }
      if (type == "date_time") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        TRY_RESULT(unix_time, object.get_required_int_field("unix_time"));
        TRY_RESULT(format, object.get_optional_string_field("date_time_format"));
        TRY_RESULT(formatting_type, get_date_time_formatting_type(format));
        return make_object<td_api::richTextDateTime>(std::move(text), unix_time, std::move(formatting_type));
      }
      if (type == "mention") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextMention>(std::move(text), td::string());
      }
      if (type == "hashtag") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextHashtag>(std::move(text), td::string());
      }
      if (type == "cashtag") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextCashtag>(std::move(text), td::string());
      }
      if (type == "bot_command") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextBotCommand>(std::move(text), td::string());
      }
      if (type == "code") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextFixed>(std::move(text));
      }
      if (type == "text_mention") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        TRY_RESULT(user, object.extract_required_field("user", td::JsonValue::Type::Object));
        CHECK(user.type() == td::JsonValue::Type::Object);
        const auto &user_object = user.get_object();
        TRY_RESULT(user_id, user_object.get_required_long_field("id"));
        return make_object<td_api::richTextMentionName>(std::move(text), user_id);
      }
      if (type == "url") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        TRY_RESULT(url, object.get_required_string_field("url"));
        return make_object<td_api::richTextUrl>(std::move(text), url, false);
      }
      if (type == "email_address") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        TRY_RESULT(email_address, object.get_required_string_field("email_address"));
        return make_object<td_api::richTextEmailAddress>(std::move(text), email_address);
      }
      if (type == "bank_card_number") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextBankCardNumber>(std::move(text), td::string());
      }
      if (type == "subscript") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextSubscript>(std::move(text));
      }
      if (type == "superscript") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextSuperscript>(std::move(text));
      }
      if (type == "marked") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        return make_object<td_api::richTextMarked>(std::move(text));
      }
      if (type == "phone_number") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        TRY_RESULT(phone_number, object.get_required_string_field("phone_number"));
        return make_object<td_api::richTextPhoneNumber>(std::move(text), phone_number);
      }
      if (type == "custom_emoji") {
        TRY_RESULT(custom_emoji_id, object.get_required_long_field("custom_emoji_id"));
        TRY_RESULT(alternative_text, object.get_required_string_field("alternative_text"));
        return make_object<td_api::richTextCustomEmoji>(custom_emoji_id, alternative_text);
      }
      if (type == "mathematical_expression") {
        TRY_RESULT(expression, object.get_required_string_field("expression"));
        return make_object<td_api::richTextMathematicalExpression>(expression);
      }
      if (type == "reference") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        TRY_RESULT(name, object.get_required_string_field("name"));
        return make_object<td_api::richTextReference>(name, std::move(text));
      }
      if (type == "reference_link") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        TRY_RESULT(reference_name, object.get_required_string_field("reference_name"));
        return make_object<td_api::richTextReferenceLink>(std::move(text), reference_name, td::string());
      }
      if (type == "anchor") {
        TRY_RESULT(name, object.get_required_string_field("name"));
        return make_object<td_api::richTextAnchor>(name);
      }
      if (type == "anchor_link") {
        TRY_RESULT(text, get_rich_text(object.extract_field("text")));
        TRY_RESULT(anchor_name, object.get_required_string_field("anchor_name"));
        return make_object<td_api::richTextAnchorLink>(std::move(text), anchor_name, td::string());
      }
      if (type == "button") {
        TRY_RESULT(button, get_inline_button(object.extract_field("button")));
        return make_object<td_api::richTextButton>(std::move(button));
      }
      return td::Status::Error(400, "Unsupported rich text type");
    }
    default:
      return td::Status::Error(400, "Invalid rich text specified");
  }
}

td::Result<td_api::object_ptr<td_api::inputPageBlockListItem>> Client::get_input_page_block_list_item(
    const Query *query, td::JsonValue &&value) const {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Object expected as InputRichBlockListItem");
  }
  auto &object = value.get_object();
  TRY_RESULT(blocks, get_input_page_blocks(query, object.extract_field("blocks")));
  TRY_RESULT(has_checkbox, object.get_optional_bool_field("has_checkbox"));
  TRY_RESULT(is_checked, object.get_optional_bool_field("is_checked"));
  TRY_RESULT(item_value, object.get_optional_int_field("value"));
  TRY_RESULT(type, object.get_optional_string_field("type"));
  return make_object<td_api::inputPageBlockListItem>(std::move(blocks), has_checkbox, is_checked, item_value, type);
}

td::Result<td_api::object_ptr<td_api::InputPageBlock>> Client::get_input_page_block(const Query *query,
                                                                                    td::JsonValue &&value) const {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Object expected as InputRichMessageBlock");
  }
  auto &object = value.get_object();
  TRY_RESULT(type, object.get_required_string_field("type"));
  if (type == "heading") {
    TRY_RESULT(text, get_rich_text(object.extract_field("text")));
    TRY_RESULT(size, object.get_required_int_field("size"));
    return make_object<td_api::inputPageBlockSectionHeading>(std::move(text), size);
  }
  if (type == "paragraph") {
    TRY_RESULT(text, get_rich_text(object.extract_field("text")));
    return make_object<td_api::inputPageBlockParagraph>(std::move(text));
  }
  if (type == "pre") {
    TRY_RESULT(text, get_rich_text(object.extract_field("text")));
    TRY_RESULT(language, object.get_optional_string_field("language"));
    return make_object<td_api::inputPageBlockPreformatted>(std::move(text), language);
  }
  if (type == "footer") {
    TRY_RESULT(text, get_rich_text(object.extract_field("text")));
    return make_object<td_api::inputPageBlockFooter>(std::move(text));
  }
  if (type == "thinking") {
    TRY_RESULT(text, get_rich_text(object.extract_field("text")));
    return make_object<td_api::inputPageBlockThinking>(std::move(text));
  }
  if (type == "divider") {
    return make_object<td_api::inputPageBlockDivider>();
  }
  if (type == "mathematical_expression") {
    TRY_RESULT(expression, object.get_required_string_field("expression"));
    return make_object<td_api::inputPageBlockMathematicalExpression>(expression);
  }
  if (type == "anchor") {
    TRY_RESULT(name, object.get_required_string_field("name"));
    return make_object<td_api::inputPageBlockAnchor>(name);
  }
  if (type == "list") {
    TRY_RESULT(items, get_array_generic<td_api::inputPageBlockListItem>(
                          object.extract_field("items"), "InputRichBlockListItem", [&](td::JsonValue &&item) {
                            return get_input_page_block_list_item(query, std::move(item));
                          }));
    return make_object<td_api::inputPageBlockList>(std::move(items));
  }
  if (type == "blockquote") {
    TRY_RESULT(blocks, get_input_page_blocks(query, object.extract_field("blocks")));
    TRY_RESULT(credit, get_rich_text(object.extract_field("credit")));
    return make_object<td_api::inputPageBlockBlockQuote>(std::move(blocks), std::move(credit));
  }
  if (type == "expandable_blockquote") {
    TRY_RESULT(text, get_rich_text(object.extract_field("text")));
    TRY_RESULT(credit, get_rich_text(object.extract_field("credit")));
    return make_object<td_api::inputPageBlockExpandableBlockQuote>(std::move(text), std::move(credit));
  }
  if (type == "pullquote") {
    TRY_RESULT(text, get_rich_text(object.extract_field("text")));
    TRY_RESULT(credit, get_rich_text(object.extract_field("credit")));
    return make_object<td_api::inputPageBlockPullQuote>(std::move(text), std::move(credit));
  }
  if (type == "collage") {
    TRY_RESULT(blocks, get_input_page_blocks(query, object.extract_field("blocks")));
    TRY_RESULT(caption, get_page_block_caption(object.extract_field("caption")));
    return make_object<td_api::inputPageBlockCollage>(std::move(blocks), std::move(caption));
  }
  if (type == "slideshow") {
    TRY_RESULT(blocks, get_input_page_blocks(query, object.extract_field("blocks")));
    TRY_RESULT(caption, get_page_block_caption(object.extract_field("caption")));
    return make_object<td_api::inputPageBlockSlideshow>(std::move(blocks), std::move(caption));
  }
  if (type == "table") {
    td::vector<td::vector<object_ptr<td_api::pageBlockTableCell>>> cells;
    TRY_RESULT(cells_array, object.extract_required_field("cells", td::JsonValue::Type::Array));
    for (auto &row : cells_array.get_array()) {
      TRY_RESULT(new_row, get_array(std::move(row), "PageBlockTableCell", get_page_block_table_cell));
      cells.push_back(std::move(new_row));
    }
    TRY_RESULT(caption, get_rich_text(object.extract_field("caption")));
    TRY_RESULT(is_bordered, object.get_optional_bool_field("is_bordered"));
    TRY_RESULT(is_striped, object.get_optional_bool_field("is_striped"));
    TRY_RESULT(is_compact, object.get_optional_bool_field("is_compact"));
    return make_object<td_api::inputPageBlockTable>(std::move(caption), std::move(cells), is_bordered, is_striped,
                                                    is_compact);
  }
  if (type == "details") {
    TRY_RESULT(summary, get_rich_text(object.extract_field("summary")));
    TRY_RESULT(blocks, get_input_page_blocks(query, object.extract_field("blocks")));
    TRY_RESULT(is_open, object.get_optional_bool_field("is_open"));
    return make_object<td_api::inputPageBlockDetails>(std::move(summary), std::move(blocks), is_open);
  }
  if (type == "map") {
    TRY_RESULT(location_field, object.extract_required_field("location", td::JsonValue::Type::Object));
    auto &location_object = location_field.get_object();
    TRY_RESULT(location, get_location(location_object));
    TRY_RESULT(zoom, object.get_optional_int_field("zoom"));
    TRY_RESULT(width, object.get_optional_int_field("width"));
    TRY_RESULT(height, object.get_optional_int_field("height"));
    TRY_RESULT(caption, get_page_block_caption(object.extract_field("caption")));
    return make_object<td_api::inputPageBlockMap>(std::move(location), zoom, width, height, std::move(caption));
  }
  if (type == "buttons") {
    TRY_RESULT(input_buttons, object.extract_required_field("buttons", td::JsonValue::Type::Array));
    TRY_RESULT(buttons, get_array(std::move(input_buttons), "RichMessageButton", get_inline_button));
    object_ptr<td_api::PageBlockHorizontalAlignment> align;
    TRY_RESULT(align_str, object.get_optional_string_field("align"));
    if (align_str.empty()) {
      // ok
    } else if (align_str == "left") {
      align = make_object<td_api::pageBlockHorizontalAlignmentLeft>();
    } else if (align_str == "center") {
      align = make_object<td_api::pageBlockHorizontalAlignmentCenter>();
    } else if (align_str == "right") {
      align = make_object<td_api::pageBlockHorizontalAlignmentRight>();
    } else {
      return td::Status::Error(400, "Invalid horizontal alignment specified");
    }
    return make_object<td_api::inputPageBlockButtonRow>(std::move(buttons), std::move(align));
  }
  if (type != "animation" && type != "audio" && type != "document" && type != "photo" && type != "video" &&
      type != "voice_note") {
    return td::Status::Error(400, PSLICE() << "type \"" << type << "\" is unsupported");
  }
  TRY_RESULT(media_field, object.extract_required_field(type, td::JsonValue::Type::Object));
  auto &media_object = media_field.get_object();
  TRY_RESULT(media_type, media_object.get_required_string_field("type"));
  if (media_type != type) {
    return td::Status::Error(400, PSLICE()
                                      << "Unexpected media type \"" << media_type << "\" for block \"" << type << "\"");
  }
  TRY_RESULT(input_message_content, get_input_media(query, media_object, type, nullptr, false, false, false, true));
  TRY_RESULT(caption, get_page_block_caption(object.extract_field("caption")));
  TRY_RESULT(has_spoiler, media_object.get_optional_bool_field("has_spoiler"));
  if (type == "animation") {
    CHECK(input_message_content->get_id() == td_api::inputMessageAnimation::ID);
    return make_object<td_api::inputPageBlockAnimation>(
        std::move(static_cast<td_api::inputMessageAnimation *>(input_message_content.get())->animation_),
        std::move(caption), has_spoiler);
  }
  if (type == "audio") {
    CHECK(input_message_content->get_id() == td_api::inputMessageAudio::ID);
    return make_object<td_api::inputPageBlockAudio>(
        std::move(static_cast<td_api::inputMessageAudio *>(input_message_content.get())->audio_), std::move(caption));
  }
  if (type == "document") {
    CHECK(input_message_content->get_id() == td_api::inputMessageDocument::ID);
    return make_object<td_api::inputPageBlockDocument>(
        std::move(static_cast<td_api::inputMessageDocument *>(input_message_content.get())->document_),
        std::move(caption));
  }
  if (type == "photo") {
    CHECK(input_message_content->get_id() == td_api::inputMessagePhoto::ID);
    return make_object<td_api::inputPageBlockPhoto>(
        std::move(static_cast<td_api::inputMessagePhoto *>(input_message_content.get())->photo_), std::move(caption),
        has_spoiler);
  }
  if (type == "video") {
    CHECK(input_message_content->get_id() == td_api::inputMessageVideo::ID);
    return make_object<td_api::inputPageBlockVideo>(
        std::move(static_cast<td_api::inputMessageVideo *>(input_message_content.get())->video_), std::move(caption),
        has_spoiler);
  }
  if (type == "voice_note") {
    CHECK(input_message_content->get_id() == td_api::inputMessageVoiceNote::ID);
    return make_object<td_api::inputPageBlockVoiceNote>(
        std::move(static_cast<td_api::inputMessageVoiceNote *>(input_message_content.get())->voice_note_),
        std::move(caption));
  }
  UNREACHABLE();
}

td::Result<td::vector<td_api::object_ptr<td_api::InputPageBlock>>> Client::get_input_page_blocks(
    const Query *query, td::JsonValue &&value) const {
  return get_array_generic<td_api::InputPageBlock>(std::move(value), "InputRichBlock", [&](td::JsonValue &&block) {
    return get_input_page_block(query, std::move(block));
  });
}

td::Result<td_api::object_ptr<td_api::inputRichMessage>> Client::get_input_rich_message(const Query *query) const {
  auto rich_message = query->arg("rich_message");
  if (rich_message.empty()) {
    return td::Status::Error(400, "Rich message must be non-empty");
  }

  LOG(INFO) << "Parsing JSON object: " << rich_message;
  auto r_value = json_decode(rich_message);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse rich message JSON object");
  }

  return get_input_rich_message(query, r_value.move_as_ok());
}

td::Result<td_api::object_ptr<td_api::inputRichMessage>> Client::get_input_rich_message(const Query *query,
                                                                                        td::JsonValue &&value) const {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Object expected as rich message");
  }
  auto &object = value.get_object();
  TRY_RESULT(is_rtl, object.get_optional_bool_field("is_rtl"));
  TRY_RESULT(skip_entity_detection, object.get_optional_bool_field("skip_entity_detection"));
  auto result = make_object<td_api::inputRichMessage>(nullptr, is_rtl, !skip_entity_detection);
  if (object.has_field("blocks")) {
    TRY_RESULT(blocks, get_input_page_blocks(query, object.extract_field("blocks")));
    result->source_ = make_object<td_api::richMessageSourceBlocks>(std::move(blocks));
  } else if (object.has_field("markdown")) {
    TRY_RESULT(text, object.get_required_string_field("markdown"));
    TRY_RESULT(media, get_input_rich_message_medias(query, object.extract_field("media")));
    result->source_ = make_object<td_api::richMessageSourceMarkdown>(text, std::move(media));
  } else if (object.has_field("html")) {
    TRY_RESULT(text, object.get_required_string_field("html"));
    TRY_RESULT(media, get_input_rich_message_medias(query, object.extract_field("media")));
    result->source_ = make_object<td_api::richMessageSourceHtml>(text, std::move(media));
  } else {
    return td::Status::Error(400, "Rich message must be non-empty");
  }
  return std::move(result);
}

td::Result<td_api::object_ptr<td_api::location>> Client::get_location(const Query *query) {
  auto latitude = trim(query->arg("latitude"));
  if (latitude.empty()) {
    return td::Status::Error(400, "Bad Request: latitude is empty");
  }
  auto longitude = trim(query->arg("longitude"));
  if (longitude.empty()) {
    return td::Status::Error(400, "Bad Request: longitude is empty");
  }
  auto horizontal_accuracy = trim(query->arg("horizontal_accuracy"));

  return make_object<td_api::location>(td::to_double(latitude), td::to_double(longitude),
                                       td::to_double(horizontal_accuracy));
}

td::Result<td_api::object_ptr<td_api::location>> Client::get_location(const td::JsonObject &object) {
  TRY_RESULT(latitude, object.get_required_double_field("latitude"));
  TRY_RESULT(longitude, object.get_required_double_field("longitude"));
  TRY_RESULT(horizontal_accuracy, object.get_optional_double_field("horizontal_accuracy"));
  return make_object<td_api::location>(latitude, longitude, horizontal_accuracy);
}

td::Result<td_api::object_ptr<td_api::venue>> Client::get_venue(const td::JsonObject &object) {
  TRY_RESULT(location, get_location(object));
  TRY_RESULT(title, object.get_required_string_field("title"));
  TRY_RESULT(address, object.get_required_string_field("address"));
  TRY_RESULT(foursquare_id, object.get_optional_string_field("foursquare_id"));
  TRY_RESULT(foursquare_type, object.get_optional_string_field("foursquare_type"));
  TRY_RESULT(google_place_id, object.get_optional_string_field("google_place_id"));
  TRY_RESULT(google_place_type, object.get_optional_string_field("google_place_type"));

  td::string provider;
  td::string venue_id;
  td::string venue_type;
  if (!google_place_id.empty() || !google_place_type.empty()) {
    provider = "gplaces";
    venue_id = std::move(google_place_id);
    venue_type = std::move(google_place_type);
  }
  if (!foursquare_id.empty() || !foursquare_type.empty()) {
    provider = "foursquare";
    venue_id = std::move(foursquare_id);
    venue_type = std::move(foursquare_type);
  }

  return make_object<td_api::venue>(std::move(location), title, address, provider, venue_id, venue_type);
}

td::Result<td_api::object_ptr<td_api::chatPermissions>> Client::get_chat_permissions(
    const Query *query, bool &allow_legacy, bool use_independent_chat_permissions) {
  auto can_send_messages = false;
  auto can_send_audios = false;
  auto can_send_documents = false;
  auto can_send_photos = false;
  auto can_send_videos = false;
  auto can_send_video_notes = false;
  auto can_send_voice_notes = false;
  auto can_send_polls = false;
  auto can_send_other_messages = false;
  auto can_add_link_previews = false;
  auto can_react_to_messages = false;
  auto can_edit_tag = false;
  auto can_change_info = false;
  auto can_invite_users = false;
  auto can_pin_messages = false;
  auto can_manage_topics = false;

  if (query->has_arg("permissions")) {
    allow_legacy = false;

    auto r_value = json_decode(query->arg("permissions"));
    if (r_value.is_error()) {
      LOG(INFO) << "Can't parse JSON object: " << r_value.error();
      return td::Status::Error(400, "Can't parse permissions JSON object");
    }

    auto value = r_value.move_as_ok();
    if (value.type() != td::JsonValue::Type::Object) {
      return td::Status::Error(400, "Object expected as permissions");
    }
    auto &object = value.get_object();

    auto status = [&] {
      TRY_RESULT_ASSIGN(can_send_messages, object.get_optional_bool_field("can_send_messages"));
      TRY_RESULT_ASSIGN(can_send_polls, object.get_optional_bool_field("can_send_polls"));
      TRY_RESULT_ASSIGN(can_send_other_messages, object.get_optional_bool_field("can_send_other_messages"));
      TRY_RESULT_ASSIGN(can_add_link_previews, object.get_optional_bool_field("can_add_web_page_previews"));
      TRY_RESULT_ASSIGN(can_change_info, object.get_optional_bool_field("can_change_info"));
      TRY_RESULT_ASSIGN(can_invite_users, object.get_optional_bool_field("can_invite_users"));
      TRY_RESULT_ASSIGN(can_pin_messages, object.get_optional_bool_field("can_pin_messages"));
      if (object.has_field("can_manage_topics")) {
        TRY_RESULT_ASSIGN(can_manage_topics, object.get_optional_bool_field("can_manage_topics"));
      } else {
        can_manage_topics = can_pin_messages;
      }
      if (object.has_field("can_send_audios") || object.has_field("can_send_documents") ||
          object.has_field("can_send_photos") || object.has_field("can_send_videos") ||
          object.has_field("can_send_video_notes") || object.has_field("can_send_voice_notes")) {
        TRY_RESULT_ASSIGN(can_send_audios, object.get_optional_bool_field("can_send_audios"));
        TRY_RESULT_ASSIGN(can_send_documents, object.get_optional_bool_field("can_send_documents"));
        TRY_RESULT_ASSIGN(can_send_photos, object.get_optional_bool_field("can_send_photos"));
        TRY_RESULT_ASSIGN(can_send_videos, object.get_optional_bool_field("can_send_videos"));
        TRY_RESULT_ASSIGN(can_send_video_notes, object.get_optional_bool_field("can_send_video_notes"));
        TRY_RESULT_ASSIGN(can_send_voice_notes, object.get_optional_bool_field("can_send_voice_notes"));
      } else {
        TRY_RESULT(can_send_media_messages, object.get_optional_bool_field("can_send_media_messages"));
        can_send_audios = can_send_media_messages;
        can_send_documents = can_send_media_messages;
        can_send_photos = can_send_media_messages;
        can_send_videos = can_send_media_messages;
        can_send_video_notes = can_send_media_messages;
        can_send_voice_notes = can_send_media_messages;
        if (can_send_media_messages && !use_independent_chat_permissions) {
          can_send_messages = true;
        }
      }
      if (object.has_field("can_edit_tag")) {
        TRY_RESULT_ASSIGN(can_edit_tag, object.get_optional_bool_field("can_edit_tag"));
      } else {
        can_edit_tag = can_pin_messages;
      }
      if (object.has_field("can_react_to_messages")) {
        TRY_RESULT_ASSIGN(can_react_to_messages, object.get_optional_bool_field("can_react_to_messages"));
      } else {
        can_react_to_messages = can_send_messages;
      }
      return td::Status::OK();
    }();

    if (status.is_error()) {
      return td::Status::Error(400, PSLICE() << "Can't parse chat permissions: " << status.message());
    }

    if ((can_send_other_messages || can_add_link_previews) && !use_independent_chat_permissions) {
      can_send_audios = true;
      can_send_documents = true;
      can_send_photos = true;
      can_send_videos = true;
      can_send_video_notes = true;
      can_send_voice_notes = true;
      can_send_messages = true;
    }
    if (can_send_polls && !use_independent_chat_permissions) {
      can_send_messages = true;
    }
  } else if (allow_legacy) {
    allow_legacy = false;

    can_send_messages = to_bool(query->arg("can_send_messages"));
    bool can_send_media_messages = to_bool(query->arg("can_send_media_messages"));
    can_send_other_messages = to_bool(query->arg("can_send_other_messages"));
    can_add_link_previews = to_bool(query->arg("can_add_web_page_previews"));
    if ((can_send_other_messages || can_add_link_previews) && !use_independent_chat_permissions) {
      can_send_media_messages = true;
    }
    if (can_send_media_messages && !use_independent_chat_permissions) {
      can_send_messages = true;
    }

    if (can_send_messages && can_send_media_messages && can_send_other_messages && can_add_link_previews) {
      // legacy unrestrict
      can_send_polls = true;
      can_change_info = true;
      can_invite_users = true;
      can_pin_messages = true;
      can_manage_topics = true;
    } else if (query->has_arg("can_send_messages") || query->has_arg("can_send_media_messages") ||
               query->has_arg("can_send_other_messages") || query->has_arg("can_add_web_page_previews")) {
      allow_legacy = true;
    }

    can_send_audios = can_send_media_messages;
    can_send_documents = can_send_media_messages;
    can_send_photos = can_send_media_messages;
    can_send_videos = can_send_media_messages;
    can_send_video_notes = can_send_media_messages;
    can_send_voice_notes = can_send_media_messages;
    can_edit_tag = can_pin_messages;
    can_react_to_messages = can_send_messages;
  }

  return make_object<td_api::chatPermissions>(
      can_send_messages, can_send_audios, can_send_documents, can_send_photos, can_send_videos, can_send_video_notes,
      can_send_voice_notes, can_send_polls, can_send_other_messages, can_add_link_previews, can_react_to_messages,
      can_edit_tag, can_change_info, can_invite_users, can_pin_messages, can_manage_topics);
}

td::Result<td_api::object_ptr<td_api::inputAnimation>> Client::get_input_animation(
    const td::JsonObject &object, object_ptr<td_api::InputFile> &&input_file,
    object_ptr<td_api::inputThumbnail> &&input_thumbnail) {
  TRY_RESULT(width, object.get_optional_int_field("width"));
  TRY_RESULT(height, object.get_optional_int_field("height"));
  TRY_RESULT(duration, object.get_optional_int_field("duration"));
  width = td::clamp(width, 0, MAX_LENGTH);
  height = td::clamp(height, 0, MAX_LENGTH);
  duration = td::clamp(duration, 0, MAX_DURATION);
  return make_object<td_api::inputAnimation>(std::move(input_file), std::move(input_thumbnail), td::vector<int32>(),
                                             duration, width, height);
}

td::Result<td_api::object_ptr<td_api::inputAudio>> Client::get_input_audio(
    const td::JsonObject &object, object_ptr<td_api::InputFile> &&input_file,
    object_ptr<td_api::inputThumbnail> &&input_thumbnail) {
  TRY_RESULT(duration, object.get_optional_int_field("duration"));
  TRY_RESULT(title, object.get_optional_string_field("title"));
  TRY_RESULT(performer, object.get_optional_string_field("performer"));
  duration = td::clamp(duration, 0, MAX_DURATION);
  return make_object<td_api::inputAudio>(std::move(input_file), std::move(input_thumbnail), duration, title, performer);
}

td::Result<td_api::object_ptr<td_api::inputPhoto>> Client::get_input_photo(const Query *query,
                                                                           const td::JsonObject &object,
                                                                           object_ptr<td_api::InputFile> &&input_file,
                                                                           bool is_live) const {
  if (is_live) {
    TRY_RESULT(photo, object.get_optional_string_field("photo"));
    auto input_photo = get_input_file(query, td::Slice(), photo, false);
    if (input_photo == nullptr) {
      return td::Status::Error("Photo not found");
    }
    return make_object<td_api::inputPhoto>(std::move(input_photo), nullptr, std::move(input_file), td::vector<int32>(),
                                           0, 0);
  }
  return make_object<td_api::inputPhoto>(std::move(input_file), nullptr, nullptr, td::vector<int32>(), 0, 0);
}

td::Result<td_api::object_ptr<td_api::inputVideo>> Client::get_input_video(
    const Query *query, const td::JsonObject &object, object_ptr<td_api::InputFile> &&input_file,
    object_ptr<td_api::inputThumbnail> &&input_thumbnail) const {
  TRY_RESULT(width, object.get_optional_int_field("width"));
  TRY_RESULT(height, object.get_optional_int_field("height"));
  TRY_RESULT(duration, object.get_optional_int_field("duration"));
  TRY_RESULT(cover, object.get_optional_string_field("cover"));
  TRY_RESULT(start_timestamp, object.get_optional_int_field("start_timestamp"));
  TRY_RESULT(supports_streaming, object.get_optional_bool_field("supports_streaming"));
  auto input_cover = get_input_file(query, td::Slice(), cover, false);
  width = td::clamp(width, 0, MAX_LENGTH);
  height = td::clamp(height, 0, MAX_LENGTH);
  duration = td::clamp(duration, 0, MAX_DURATION);
  start_timestamp = td::clamp(start_timestamp, 0, MAX_DURATION);
  return make_object<td_api::inputVideo>(std::move(input_file), std::move(input_thumbnail), std::move(input_cover),
                                         start_timestamp, td::vector<int32>(), duration, width, height,
                                         supports_streaming);
}

td::Result<td_api::object_ptr<td_api::inputVoiceNote>> Client::get_input_voice_note(
    const td::JsonObject &object, object_ptr<td_api::InputFile> &&input_file) {
  TRY_RESULT(duration, object.get_optional_int_field("duration"));
  duration = td::clamp(duration, 0, MAX_DURATION);
  return make_object<td_api::inputVoiceNote>(std::move(input_file), duration, td::string());
}

td::Result<td_api::object_ptr<td_api::inputChecklistTask>> Client::get_input_checklist_task(
    td::JsonValue &&input_task) {
  if (input_task.type() != td::JsonValue::Type::Object) {
    return td::Status::Error("expected an Object");
  }

  auto &object = input_task.get_object();
  TRY_RESULT(id, object.get_required_int_field("id"));
  TRY_RESULT(input_text, object.get_required_string_field("text"));
  TRY_RESULT(parse_mode, object.get_optional_string_field("parse_mode"));
  auto entities = object.extract_field("text_entities");
  TRY_RESULT(text, get_formatted_text(std::move(input_text), std::move(parse_mode), std::move(entities)));

  return make_object<td_api::inputChecklistTask>(id, std::move(text));
}

td::Result<td_api::object_ptr<td_api::inputChecklist>> Client::get_input_checklist(
    const Query *query, td::JsonValue &&input_checklist) const {
  if (input_checklist.type() != td::JsonValue::Type::Object) {
    return td::Status::Error("expected an Object");
  }

  auto &object = input_checklist.get_object();
  TRY_RESULT(input_title, object.get_required_string_field("title"));
  TRY_RESULT(parse_mode, object.get_optional_string_field("parse_mode"));
  auto entities = object.extract_field("title_entities");
  TRY_RESULT(title, get_formatted_text(std::move(input_title), std::move(parse_mode), std::move(entities)));
  TRY_RESULT(others_can_add_tasks, object.get_optional_bool_field("others_can_add_tasks"));
  TRY_RESULT(others_can_mark_tasks_as_done, object.get_optional_bool_field("others_can_mark_tasks_as_done"));
  TRY_RESULT(input_tasks, object.extract_required_field("tasks", td::JsonValue::Type::Array));
  TRY_RESULT(tasks, get_array(std::move(input_tasks), "InputChecklistTask", get_input_checklist_task));

  return make_object<td_api::inputChecklist>(std::move(title), std::move(tasks), others_can_add_tasks,
                                             others_can_mark_tasks_as_done);
}

td::Result<td_api::object_ptr<td_api::inputChecklist>> Client::get_input_checklist(const Query *query,
                                                                                   td::Slice field_name) const {
  TRY_RESULT(checklist, get_required_string_arg(query, field_name));

  LOG(INFO) << "Parsing JSON object: " << checklist;
  auto r_value = json_decode(checklist);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse InputChecklist JSON object");
  }

  auto r_input_checklist = get_input_checklist(query, r_value.move_as_ok());
  if (r_input_checklist.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse InputChecklist: " << r_input_checklist.error().message());
  }
  return r_input_checklist.move_as_ok();
}

td::Result<td_api::object_ptr<td_api::InputMessageContent>> Client::get_input_media(const Query *query,
                                                                                    td::JsonValue &&input_media,
                                                                                    bool for_album) const {
  if (input_media.type() != td::JsonValue::Type::Object) {
    return td::Status::Error("expected an Object");
  }

  auto &object = input_media.get_object();

  TRY_RESULT(input_caption, object.get_optional_string_field("caption"));
  TRY_RESULT(parse_mode, object.get_optional_string_field("parse_mode"));
  auto entities = object.extract_field("caption_entities");
  TRY_RESULT(caption, get_formatted_text(std::move(input_caption), std::move(parse_mode), std::move(entities)));
  TRY_RESULT(show_caption_above_media, object.get_optional_bool_field("show_caption_above_media"));
  TRY_RESULT(has_spoiler, object.get_optional_bool_field("has_spoiler"));
  TRY_RESULT(type, object.get_required_string_field("type"));

  return get_input_media(query, object, type, std::move(caption), show_caption_above_media, has_spoiler, for_album,
                         false);
}

td::Result<td_api::object_ptr<td_api::InputMessageContent>> Client::get_input_media(
    const Query *query, const td::JsonObject &object, const td::string &type,
    object_ptr<td_api::formattedText> &&caption, bool show_caption_above_media, bool has_spoiler, bool for_album,
    bool for_rich_message) const {
  TRY_RESULT(media, object.get_optional_string_field("media"));

  auto input_file = get_input_file(query, td::Slice(), media, false);
  if (input_file == nullptr) {
    return td::Status::Error("media not found");
  }
  TRY_RESULT(input_thumbnail, get_input_thumbnail(query, object, true));

  if (type == "photo" || type == "live_photo") {
    TRY_RESULT(input_photo, get_input_photo(query, object, std::move(input_file), type == "live_photo"));
    return make_object<td_api::inputMessagePhoto>(std::move(input_photo), std::move(caption), show_caption_above_media,
                                                  nullptr, has_spoiler);
  }
  if (type == "video") {
    TRY_RESULT(input_video, get_input_video(query, object, std::move(input_file), std::move(input_thumbnail)));
    return make_object<td_api::inputMessageVideo>(std::move(input_video), std::move(caption), show_caption_above_media,
                                                  nullptr, has_spoiler);
  }
  if (type == "animation") {
    if (for_album) {
      return td::Status::Error(PSLICE() << "type \"" << type << "\" can't be used in sendMediaGroup");
    }
    TRY_RESULT(input_animation, get_input_animation(object, std::move(input_file), std::move(input_thumbnail)));
    return make_object<td_api::inputMessageAnimation>(std::move(input_animation), std::move(caption),
                                                      show_caption_above_media, has_spoiler);
  }
  if (type == "audio") {
    TRY_RESULT(input_audio, get_input_audio(object, std::move(input_file), std::move(input_thumbnail)));
    return make_object<td_api::inputMessageAudio>(std::move(input_audio), std::move(caption));
  }
  if (type == "document") {
    TRY_RESULT(disable_content_type_detection, object.get_optional_bool_field("disable_content_type_detection"));
    return make_object<td_api::inputMessageDocument>(
        make_object<td_api::inputDocument>(std::move(input_file), std::move(input_thumbnail),
                                           disable_content_type_detection || for_album),
        std::move(caption));
  }
  if (type == "voice_note") {
    if (!for_rich_message) {
      return td::Status::Error(PSLICE() << "type \"" << type << "\" is not allowed");
    }
    TRY_RESULT(input_voice_note, get_input_voice_note(object, std::move(input_file)));
    return make_object<td_api::inputMessageVoiceNote>(std::move(input_voice_note), std::move(caption), nullptr);
  }

  return td::Status::Error(PSLICE() << "type \"" << type << "\" is unsupported");
}

td::Result<td_api::object_ptr<td_api::InputMessageContent>> Client::get_input_media(const Query *query,
                                                                                    td::Slice field_name) const {
  TRY_RESULT(media, get_required_string_arg(query, field_name));

  LOG(INFO) << "Parsing JSON object: " << media;
  auto r_value = json_decode(media);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse input media JSON object");
  }

  auto r_input_message_content = get_input_media(query, r_value.move_as_ok(), false);
  if (r_input_message_content.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse InputMedia: " << r_input_message_content.error().message());
  }
  return r_input_message_content.move_as_ok();
}

td::Result<td_api::object_ptr<td_api::InputPollMedia>> Client::get_input_poll_media(const Query *query,
                                                                                    td::JsonValue &&input_media,
                                                                                    bool for_option) const {
  if (input_media.type() != td::JsonValue::Type::Object) {
    if (input_media.type() == td::JsonValue::Type::Null) {
      return nullptr;
    }
    return td::Status::Error("expected an Object");
  }

  auto &object = input_media.get_object();

  TRY_RESULT(type, object.get_required_string_field("type"));
  if (type == "location") {
    TRY_RESULT(location, get_location(object));
    return make_object<td_api::inputPollMediaLocation>(std::move(location));
  }
  if (type == "venue") {
    TRY_RESULT(venue, get_venue(object));
    return make_object<td_api::inputPollMediaVenue>(std::move(venue));
  }
  if (type == "link") {
    if (!for_option) {
      return td::Status::Error(PSLICE() << "type \"" << type << "\" is unsupported");
    }
    TRY_RESULT(url, object.get_required_string_field("url"));
    return make_object<td_api::inputPollMediaLink>(url);
  }

  TRY_RESULT(media, object.get_optional_string_field("media"));
  auto input_file = get_input_file(query, td::Slice(), media, false);
  if (input_file == nullptr) {
    return td::Status::Error("media not found");
  }
  TRY_RESULT(input_thumbnail, get_input_thumbnail(query, object, true));

  if (type == "photo" || type == "live_photo") {
    TRY_RESULT(input_photo, get_input_photo(query, object, std::move(input_file), type == "live_photo"));
    return make_object<td_api::inputPollMediaPhoto>(std::move(input_photo));
  }
  if (type == "video") {
    TRY_RESULT(input_video, get_input_video(query, object, std::move(input_file), std::move(input_thumbnail)));
    return make_object<td_api::inputPollMediaVideo>(std::move(input_video));
  }
  if (type == "animation") {
    TRY_RESULT(input_animation, get_input_animation(object, std::move(input_file), std::move(input_thumbnail)));
    return make_object<td_api::inputPollMediaAnimation>(std::move(input_animation));
  }

  if (for_option ? type != "sticker" : type != "audio" && type != "document") {
    return td::Status::Error(PSLICE() << "type \"" << type << "\" is unsupported");
  }
  if (type == "sticker") {
    return make_object<td_api::inputPollMediaSticker>(
        make_object<td_api::inputSticker>(std::move(input_file), std::move(input_thumbnail), 0, 0));
  }
  if (type == "audio") {
    TRY_RESULT(input_audio, get_input_audio(object, std::move(input_file), std::move(input_thumbnail)));
    return make_object<td_api::inputPollMediaAudio>(std::move(input_audio));
  }
  if (type == "document") {
    TRY_RESULT(disable_content_type_detection, object.get_optional_bool_field("disable_content_type_detection"));
    return make_object<td_api::inputPollMediaDocument>(make_object<td_api::inputDocument>(
        std::move(input_file), std::move(input_thumbnail), disable_content_type_detection));
  }
  UNREACHABLE();
}

td::Result<td_api::object_ptr<td_api::InputPollMedia>> Client::get_input_poll_media(const Query *query,
                                                                                    td::Slice field_name) const {
  auto media = query->arg(field_name);
  if (media.empty()) {
    return nullptr;
  }

  LOG(INFO) << "Parsing JSON object: " << media;
  auto r_value = json_decode(media);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse InputPollMedia JSON object");
  }

  auto r_input_message_content = get_input_poll_media(query, r_value.move_as_ok(), false);
  if (r_input_message_content.is_error()) {
    return td::Status::Error(
        400, PSLICE() << "Failed to parse InputPollMedia: " << r_input_message_content.error().message());
  }
  return r_input_message_content.move_as_ok();
}

td::Result<td_api::object_ptr<td_api::inputRichMessageMedia>> Client::get_input_rich_message_media(
    const Query *query, td::JsonValue &&input_media) const {
  if (input_media.type() != td::JsonValue::Type::Object) {
    return td::Status::Error("expected an Object");
  }

  auto &object = input_media.get_object();
  TRY_RESULT(id, object.get_required_string_field("id"));
  TRY_RESULT(media, object.extract_required_field("media", td::JsonValue::Type::Object));
  auto &media_object = media.get_object();
  TRY_RESULT(type, media_object.get_required_string_field("type"));
  TRY_RESULT(input_message_content, get_input_media(query, media_object, type, nullptr, false, false, false, true));
  return make_object<td_api::inputRichMessageMedia>(id, std::move(input_message_content));
}

td::Result<td::vector<td_api::object_ptr<td_api::inputRichMessageMedia>>> Client::get_input_rich_message_medias(
    const Query *query, td::JsonValue &&value) const {
  return get_array_generic<td_api::inputRichMessageMedia>(
      std::move(value), "InputRichMessageMedia",
      [&](td::JsonValue &&input_media) { return get_input_rich_message_media(query, std::move(input_media)); });
}

td::Result<td::vector<td_api::object_ptr<td_api::InputMessageContent>>> Client::get_input_message_contents(
    const Query *query, td::Slice field_name) const {
  TRY_RESULT(media, get_required_string_arg(query, field_name));

  LOG(INFO) << "Parsing JSON object: " << media;
  auto r_value = json_decode(media);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse media JSON object");
  }

  return get_array_generic<td_api::InputMessageContent>(r_value.move_as_ok(), "InputMedia", [&](td::JsonValue &&value) {
    return get_input_media(query, std::move(value), true);
  });
}

td::Result<td_api::object_ptr<td_api::inputPaidMedia>> Client::get_input_paid_media(const Query *query,
                                                                                    td::JsonValue &&input_media) const {
  if (input_media.type() != td::JsonValue::Type::Object) {
    return td::Status::Error("expected an Object");
  }

  auto &object = input_media.get_object();
  TRY_RESULT(media, object.get_optional_string_field("media"));

  auto input_file = get_input_file(query, td::Slice(), media, false);
  if (input_file == nullptr) {
    return td::Status::Error("media not found");
  }
  TRY_RESULT(input_thumbnail, get_input_thumbnail(query, object, true));

  TRY_RESULT(width, object.get_optional_int_field("width"));
  TRY_RESULT(height, object.get_optional_int_field("height"));
  width = td::clamp(width, 0, MAX_LENGTH);
  height = td::clamp(height, 0, MAX_LENGTH);

  object_ptr<td_api::InputPaidMediaType> media_type;
  TRY_RESULT(type, object.get_required_string_field("type"));
  if (type == "photo") {
    media_type = make_object<td_api::inputPaidMediaTypePhoto>(nullptr);
  } else if (type == "live_photo") {
    TRY_RESULT(photo, object.get_optional_string_field("photo"));
    media_type = make_object<td_api::inputPaidMediaTypePhoto>(std::move(input_file));
    input_file = get_input_file(query, "photo", photo, false);
    if (input_file == nullptr) {
      return td::Status::Error("Photo not found");
    }
  } else if (type == "video") {
    TRY_RESULT(duration, object.get_optional_int_field("duration"));
    TRY_RESULT(supports_streaming, object.get_optional_bool_field("supports_streaming"));
    TRY_RESULT(cover, object.get_optional_string_field("cover"));
    TRY_RESULT(start_timestamp, object.get_optional_int_field("start_timestamp"));
    auto cover_input_file = get_input_file(query, "cover", cover, false);
    duration = td::clamp(duration, 0, MAX_DURATION);
    media_type = make_object<td_api::inputPaidMediaTypeVideo>(std::move(cover_input_file), start_timestamp, duration,
                                                              supports_streaming);
  } else {
    return td::Status::Error(PSLICE() << "type \"" << type << "\" is unsupported");
  }

  return make_object<td_api::inputPaidMedia>(std::move(media_type), std::move(input_file), std::move(input_thumbnail),
                                             td::vector<int32>(), width, height);
}

td::Result<td_api::object_ptr<td_api::inputPaidMedia>> Client::get_input_paid_media(const Query *query,
                                                                                    td::Slice field_name) const {
  TRY_RESULT(media, get_required_string_arg(query, field_name));

  LOG(INFO) << "Parsing JSON object: " << media;
  auto r_value = json_decode(media);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse input paid media JSON object");
  }

  auto r_input_paid_media = get_input_paid_media(query, r_value.move_as_ok());
  if (r_input_paid_media.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse InputPaidMedia: " << r_input_paid_media.error().message());
  }
  return r_input_paid_media.move_as_ok();
}

td::Result<td::vector<td_api::object_ptr<td_api::inputPaidMedia>>> Client::get_paid_media(const Query *query,
                                                                                          td::Slice field_name) const {
  TRY_RESULT(media, get_required_string_arg(query, field_name));

  LOG(INFO) << "Parsing JSON object: " << media;
  auto r_value = json_decode(media);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse paid media JSON object");
  }

  return get_array_generic<td_api::inputPaidMedia>(
      r_value.move_as_ok(), "InputPaidMedia",
      [&](td::JsonValue &&input_media) { return get_input_paid_media(query, std::move(input_media)); });
}

td::Result<td_api::object_ptr<td_api::inputMessageInvoice>> Client::get_input_message_invoice(
    const Query *query) const {
  TRY_RESULT(title, get_required_string_arg(query, "title"));
  TRY_RESULT(description, get_required_string_arg(query, "description"));
  TRY_RESULT(payload, get_required_string_arg(query, "payload"));
  if (!td::check_utf8(payload.str())) {
    return td::Status::Error(400, "The payload must be encoded in UTF-8");
  }
  auto provider_token = query->arg("provider_token");
  auto provider_data = query->arg("provider_data");
  auto start_parameter = query->arg("start_parameter");
  TRY_RESULT(currency, get_required_string_arg(query, "currency"));

  TRY_RESULT(labeled_price_parts, get_required_string_arg(query, "prices"));
  auto r_labeled_price_parts_value = json_decode(labeled_price_parts);
  if (r_labeled_price_parts_value.is_error()) {
    return td::Status::Error(400, "Can't parse prices JSON object");
  }

  TRY_RESULT(prices, get_labeled_price_parts(r_labeled_price_parts_value.move_as_ok()));

  int64 max_tip_amount = 0;
  td::vector<int64> suggested_tip_amounts;
  {
    auto max_tip_amount_str = query->arg("max_tip_amount");
    if (!max_tip_amount_str.empty()) {
      auto r_max_tip_amount = td::to_integer_safe<int64>(max_tip_amount_str);
      if (r_max_tip_amount.is_error()) {
        return td::Status::Error(400, "Can't parse \"max_tip_amount\" as Number");
      }
      max_tip_amount = r_max_tip_amount.ok();
    }

    auto suggested_tip_amounts_str = query->arg("suggested_tip_amounts");
    if (!suggested_tip_amounts_str.empty()) {
      auto r_suggested_tip_amounts_value = json_decode(suggested_tip_amounts_str);
      if (r_suggested_tip_amounts_value.is_error()) {
        return td::Status::Error(400, "Can't parse suggested_tip_amounts JSON object");
      }

      TRY_RESULT_ASSIGN(suggested_tip_amounts, get_suggested_tip_amounts(r_suggested_tip_amounts_value.move_as_ok()));
    }
  }

  auto photo_url = query->arg("photo_url");
  int32 photo_size = get_integer_arg(query, "photo_size", 0, 0, 1000000000);
  int32 photo_width = get_integer_arg(query, "photo_width", 0, 0, MAX_LENGTH);
  int32 photo_height = get_integer_arg(query, "photo_height", 0, 0, MAX_LENGTH);

  auto need_name = to_bool(query->arg("need_name"));
  auto need_phone_number = to_bool(query->arg("need_phone_number"));
  auto need_email_address = to_bool(query->arg("need_email"));
  auto need_shipping_address = to_bool(query->arg("need_shipping_address"));
  auto send_phone_number_to_provider = to_bool(query->arg("send_phone_number_to_provider"));
  auto send_email_address_to_provider = to_bool(query->arg("send_email_to_provider"));
  auto is_flexible = to_bool(query->arg("is_flexible"));

  object_ptr<td_api::inputPaidMedia> paid_media;
  if (!query->arg("paid_media").empty()) {
    TRY_RESULT_ASSIGN(paid_media, get_input_paid_media(query, "paid_media"));
  } else if (!query->arg("extended_media").empty()) {
    TRY_RESULT_ASSIGN(paid_media, get_input_paid_media(query, "extended_media"));
  }
  TRY_RESULT(paid_media_caption, get_formatted_text(query->arg("paid_media_caption").str(),
                                                    query->arg("paid_media_caption_parse_mode").str(),
                                                    get_input_entities(query, "paid_media_caption_entities")));
  int32 subscription_period = get_integer_arg(query, "subscription_period", 0, 0);

  return make_object<td_api::inputMessageInvoice>(
      make_object<td_api::invoice>(currency.str(), std::move(prices), subscription_period, max_tip_amount,
                                   std::move(suggested_tip_amounts), td::string(), td::string(), false, need_name,
                                   need_phone_number, need_email_address, need_shipping_address,
                                   send_phone_number_to_provider, send_email_address_to_provider, is_flexible),
      title.str(), description.str(), photo_url.str(), photo_size, photo_width, photo_height, payload.str(),
      provider_token.str(), provider_data.str(), start_parameter.str(), std::move(paid_media),
      std::move(paid_media_caption));
}

td::Result<td::vector<td_api::object_ptr<td_api::inputPollOption>>> Client::get_input_poll_options(
    const Query *query) const {
  auto input_options = query->arg("options");
  LOG(INFO) << "Parsing JSON object: " << input_options;
  auto r_value = json_decode(input_options);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse options JSON object");
  }

  return get_array_generic<td_api::inputPollOption>(
      r_value.move_as_ok(), "InputPollOption",
      [&](td::JsonValue &&input_option) -> td::Result<object_ptr<td_api::inputPollOption>> {
        if (input_option.type() != td::JsonValue::Type::String) {
          if (input_option.type() == td::JsonValue::Type::Object) {
            auto &object = input_option.get_object();
            TRY_RESULT(text, object.get_required_string_field("text"));
            TRY_RESULT(parse_mode, object.get_optional_string_field("text_parse_mode"));
            TRY_RESULT(option_text, get_formatted_text(std::move(text), std::move(parse_mode),
                                                       object.extract_field("text_entities")));
            auto r_media = get_input_poll_media(query, object.extract_field("media"), true);
            if (r_media.is_error()) {
              return td::Status::Error(400, PSLICE()
                                                << "Failed to parse poll option media: " << r_media.error().message());
            }
            return make_object<td_api::inputPollOption>(std::move(option_text), r_media.move_as_ok());
          }

          return td::Status::Error(400, "Expected InputPollOption to be an Object");
        }
        return make_object<td_api::inputPollOption>(
            make_object<td_api::formattedText>(input_option.get_string().str(), td::Auto()), nullptr);
      });
}

td::Result<td_api::object_ptr<td_api::ReactionType>> Client::get_reaction_type(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "expected an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(type, object.get_required_string_field("type"));
  if (type == "emoji") {
    TRY_RESULT(emoji, object.get_required_string_field("emoji"));
    return make_object<td_api::reactionTypeEmoji>(emoji);
  }
  if (type == "custom_emoji") {
    TRY_RESULT(custom_emoji_id, object.get_required_long_field("custom_emoji_id"));
    return make_object<td_api::reactionTypeCustomEmoji>(custom_emoji_id);
  }
  return td::Status::Error(400, "invalid reaction type specified");
}

td::Result<td::vector<td_api::object_ptr<td_api::ReactionType>>> Client::get_reaction_types(const Query *query) {
  auto types = query->arg("reaction");
  if (types.empty()) {
    return td::vector<object_ptr<td_api::ReactionType>>();
  }
  LOG(INFO) << "Parsing JSON object: " << types;
  auto r_value = json_decode(types);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse reaction types JSON object");
  }

  return get_array(r_value.move_as_ok(), "ReactionType", get_reaction_type);
}

td::Result<td_api::object_ptr<td_api::InputStoryAreaType>> Client::get_input_story_area_type(td::JsonValue &&value) {
  CHECK(value.type() == td::JsonValue::Type::Object);
  auto &object = value.get_object();

  TRY_RESULT(type, object.get_required_string_field("type"));
  if (type == "location") {
    TRY_RESULT(location, get_location(object));
    object_ptr<td_api::locationAddress> location_address;
    TRY_RESULT(address, object.extract_optional_field("address", td::JsonValue::Type::Object));
    if (address.type() == td::JsonValue::Type::Object) {
      auto &address_object = address.get_object();
      TRY_RESULT(country_code, address_object.get_required_string_field("country_code"));
      TRY_RESULT(state, address_object.get_optional_string_field("state"));
      TRY_RESULT(city, address_object.get_optional_string_field("city"));
      TRY_RESULT(street, address_object.get_optional_string_field("street"));
      location_address = make_object<td_api::locationAddress>(country_code, state, city, street);
    }
    return make_object<td_api::inputStoryAreaTypeLocation>(std::move(location), std::move(location_address));
  }
  if (type == "suggested_reaction") {
    TRY_RESULT(reaction_type, object.extract_required_field("reaction_type", td::JsonValue::Type::Object));
    auto r_reaction_type = get_reaction_type(std::move(reaction_type));
    if (r_reaction_type.is_error()) {
      return td::Status::Error(400, PSLICE() << "can't parse ReactionType: " << r_reaction_type.error().message());
    }
    TRY_RESULT(is_dark, object.get_optional_bool_field("is_dark"));
    TRY_RESULT(is_flipped, object.get_optional_bool_field("is_flipped"));
    return make_object<td_api::inputStoryAreaTypeSuggestedReaction>(r_reaction_type.move_as_ok(), is_dark, is_flipped);
  }
  if (type == "link") {
    TRY_RESULT(url, object.get_required_string_field("url"));
    return make_object<td_api::inputStoryAreaTypeLink>(url);
  }
  if (type == "weather") {
    TRY_RESULT(temperature, object.get_required_double_field("temperature"));
    TRY_RESULT(emoji, object.get_required_string_field("emoji"));
    TRY_RESULT(background_color, object.get_required_long_field("background_color"));
    if (background_color < 0 || background_color > 0xFFFFFFFF) {
      return td::Status::Error(400, "invalid background color specified");
    }
    return make_object<td_api::inputStoryAreaTypeWeather>(
        temperature, emoji, static_cast<td::int32>(static_cast<td::uint32>(background_color)));
  }
  if (type == "unique_gift") {
    TRY_RESULT(name, object.get_required_string_field("name"));
    return make_object<td_api::inputStoryAreaTypeUpgradedGift>(name);
  }
  return td::Status::Error(400, "invalid story area type specified");
}

td::Result<td_api::object_ptr<td_api::inputStoryArea>> Client::get_input_story_area(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "expected an Object");
  }

  auto &object = value.get_object();

  TRY_RESULT(position, object.extract_required_field("position", td::JsonValue::Type::Object));
  auto &position_object = position.get_object();
  TRY_RESULT(x_percentage, position_object.get_required_double_field("x_percentage"));
  TRY_RESULT(y_percentage, position_object.get_required_double_field("y_percentage"));
  TRY_RESULT(width_percentage, position_object.get_required_double_field("width_percentage"));
  TRY_RESULT(height_percentage, position_object.get_required_double_field("height_percentage"));
  TRY_RESULT(rotation_angle, position_object.get_required_double_field("rotation_angle"));
  TRY_RESULT(corner_radius_percentage, position_object.get_required_double_field("corner_radius_percentage"));
  TRY_RESULT(type, object.extract_required_field("type", td::JsonValue::Type::Object));
  TRY_RESULT(input_type, get_input_story_area_type(std::move(type)));

  return make_object<td_api::inputStoryArea>(
      make_object<td_api::storyAreaPosition>(x_percentage, y_percentage, width_percentage, height_percentage,
                                             rotation_angle, corner_radius_percentage),
      std::move(input_type));
}

td::Result<td_api::object_ptr<td_api::inputStoryAreas>> Client::get_input_story_areas(const Query *query) {
  auto areas = query->arg("areas");
  if (areas.empty()) {
    return nullptr;
  }
  LOG(INFO) << "Parsing JSON object: " << areas;
  auto r_value = json_decode(areas);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse story areas JSON object");
  }

  TRY_RESULT(input_story_areas, get_array(r_value.move_as_ok(), "InputStoryArea", get_input_story_area));
  return make_object<td_api::inputStoryAreas>(std::move(input_story_areas));
}

td::Result<td_api::object_ptr<td_api::InputChatPhoto>> Client::get_input_chat_photo(const Query *query,
                                                                                    td::JsonValue &&value) const {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Object expected as profile photo");
  }
  auto &object = value.get_object();

  TRY_RESULT(type, object.get_required_string_field("type"));
  if (type == "static") {
    TRY_RESULT(photo, object.get_required_string_field("photo"));
    auto input_file = get_input_file(query, td::Slice(), photo, true);
    if (input_file == nullptr) {
      return td::Status::Error(400, "Photo must be uploaded as a file");
    }
    return make_object<td_api::inputChatPhotoStatic>(std::move(input_file));
  }
  if (type == "animated") {
    TRY_RESULT(animation, object.get_required_string_field("animation"));
    TRY_RESULT(main_frame_timestamp, object.get_optional_double_field("main_frame_timestamp"));
    auto input_file = get_input_file(query, td::Slice(), animation, true);
    if (input_file == nullptr) {
      return td::Status::Error(400, "Photo must be uploaded as a file");
    }
    return make_object<td_api::inputChatPhotoAnimation>(std::move(input_file), main_frame_timestamp);
  }
  return td::Status::Error(400, "Invalid profile photo type specified");
}

td::Result<td_api::object_ptr<td_api::InputChatPhoto>> Client::get_input_chat_photo(const Query *query) const {
  auto photo = query->arg("photo");
  if (photo.empty()) {
    return td::Status::Error(400, "Photo isn't specified");
  }

  LOG(INFO) << "Parsing JSON object: " << photo;
  auto r_value = json_decode(photo);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse photo JSON object");
  }

  return get_input_chat_photo(query, r_value.move_as_ok());
}

td::Result<td_api::object_ptr<td_api::InputStoryContent>> Client::get_input_story_content(const Query *query,
                                                                                          td::JsonValue &&value) const {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Object expected as story content");
  }
  auto &object = value.get_object();

  TRY_RESULT(type, object.get_required_string_field("type"));
  if (type == "photo") {
    TRY_RESULT(photo, object.get_required_string_field("photo"));
    auto input_file = get_input_file(query, td::Slice(), photo, true);
    if (input_file == nullptr) {
      return td::Status::Error(400, "Story photo must be uploaded as a file");
    }
    return make_object<td_api::inputStoryContentPhoto>(std::move(input_file), td::vector<int32>());
  }
  if (type == "video") {
    TRY_RESULT(video, object.get_required_string_field("video"));
    TRY_RESULT(duration, object.get_optional_double_field("duration"));
    TRY_RESULT(cover_frame_timestamp, object.get_optional_double_field("cover_frame_timestamp"));
    TRY_RESULT(is_animation, object.get_optional_bool_field("is_animation"));
    auto input_file = get_input_file(query, td::Slice(), video, true);
    if (input_file == nullptr) {
      return td::Status::Error(400, "Story video must be uploaded as a file");
    }
    return make_object<td_api::inputStoryContentVideo>(std::move(input_file), td::vector<int32>(), duration,
                                                       cover_frame_timestamp, is_animation);
  }
  return td::Status::Error(400, "Invalid story content type specified");
}

td::Result<td_api::object_ptr<td_api::InputStoryContent>> Client::get_input_story_content(const Query *query) const {
  auto content = query->arg("content");
  if (content.empty()) {
    return td::Status::Error(400, "Story content isn't specified");
  }

  LOG(INFO) << "Parsing JSON object: " << content;
  auto r_value = json_decode(content);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse story content JSON object");
  }

  return get_input_story_content(query, r_value.move_as_ok());
}

td::Result<td_api::object_ptr<td_api::acceptedGiftTypes>> Client::get_accepted_gift_types(td::JsonValue &&value) {
  if (value.type() != td::JsonValue::Type::Object) {
    return td::Status::Error(400, "Object expected as accepted gift types");
  }
  auto &object = value.get_object();

  TRY_RESULT(unlimited_gifts, object.get_required_bool_field("unlimited_gifts"));
  TRY_RESULT(limited_gifts, object.get_required_bool_field("limited_gifts"));
  TRY_RESULT(upgraded_gifts, object.get_required_bool_field("unique_gifts"));
  TRY_RESULT(premium_subscription, object.get_required_bool_field("premium_subscription"));
  bool gifts_from_channels = true;
  if (object.has_field("gifts_from_channels")) {
    TRY_RESULT_ASSIGN(gifts_from_channels, object.get_optional_bool_field("gifts_from_channels"));
  }
  return make_object<td_api::acceptedGiftTypes>(unlimited_gifts, limited_gifts, upgraded_gifts, gifts_from_channels,
                                                premium_subscription);
}

td::Result<td_api::object_ptr<td_api::acceptedGiftTypes>> Client::get_accepted_gift_types(const Query *query) {
  auto types = query->arg("accepted_gift_types");
  if (types.empty()) {
    return td::Status::Error(400, "Accepted gift types aren't specified");
  }

  LOG(INFO) << "Parsing JSON object: " << types;
  auto r_value = json_decode(types);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return td::Status::Error(400, "Can't parse accepted gift types JSON object");
  }

  return get_accepted_gift_types(r_value.move_as_ok());
}

td::int32 Client::get_integer_arg(const Query *query, td::Slice field_name, int32 default_value, int32 min_value,
                                  int32 max_value) {
  auto s_arg = query->arg(field_name);
  auto value = s_arg.empty() ? default_value : td::to_integer<int32>(s_arg);
  return td::clamp(value, min_value, max_value);
}

td::Result<td::MutableSlice> Client::get_required_string_arg(const Query *query, td::Slice field_name) {
  auto s_arg = query->arg(field_name);
  if (s_arg.empty()) {
    return td::Status::Error(400, PSLICE() << "Parameter \"" << field_name << "\" is required");
  }
  return s_arg;
}

td::int32 Client::get_forum_topic_id(const Query *query, td::Slice field_name) {
  auto s_arg = query->arg(field_name);
  if (s_arg.empty()) {
    return 0;
  }

  int arg = td::to_integer<int32>(s_arg);
  if (arg < 0) {
    return 0;
  }

  return arg;
}

td::int64 Client::get_message_id(const Query *query, td::Slice field_name) {
  auto s_arg = query->arg(field_name);
  if (s_arg.empty()) {
    return 0;
  }

  int arg = td::to_integer<int32>(s_arg);
  if (arg < 0) {
    return 0;
  }

  return as_tdlib_message_id(arg);
}

td::Result<td::vector<td::int64>> Client::get_message_ids(const Query *query, size_t max_count, td::Slice field_name) {
  auto message_ids_str = query->arg(field_name);
  if (message_ids_str.empty()) {
    return td::Status::Error(400, "Message identifiers are not specified");
  }

  auto r_value = json_decode(message_ids_str);
  if (r_value.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse " << field_name << " JSON object");
  }
  auto value = r_value.move_as_ok();
  if (value.type() != td::JsonValue::Type::Array) {
    return td::Status::Error(400, "Expected an Array of message identifiers");
  }
  if (value.get_array().size() > max_count) {
    return td::Status::Error(400, "Too many message identifiers specified");
  }

  td::vector<int64> message_ids;
  for (auto &message_id : value.get_array()) {
    td::Slice number;
    if (message_id.type() == td::JsonValue::Type::Number) {
      number = message_id.get_number();
    } else if (message_id.type() == td::JsonValue::Type::String) {
      number = message_id.get_string();
    } else {
      return td::Status::Error(400, "Message identifier must be a Number");
    }
    auto parsed_message_id = td::to_integer_safe<int32>(number);
    if (parsed_message_id.is_error()) {
      return td::Status::Error(400, "Can't parse message identifier as a Number");
    }
    if (parsed_message_id.ok() <= 0) {
      return td::Status::Error(400, "Invalid message identifier specified");
    }
    message_ids.push_back(as_tdlib_message_id(parsed_message_id.ok()));
  }
  return std::move(message_ids);
}

td::Result<td::Slice> Client::get_inline_message_id(const Query *query, td::Slice field_name) {
  auto s_arg = query->arg(field_name);
  if (s_arg.empty()) {
    return td::Status::Error(400, "Message identifier is not specified");
  }
  return s_arg;
}

td::Result<td::int64> Client::get_user_id(const Query *query, td::Slice field_name) {
  int64 user_id = td::max(td::to_integer<int64>(query->arg(field_name)), static_cast<int64>(0));
  if (user_id == 0) {
    return td::Status::Error(400, PSLICE() << "Invalid " << field_name << " specified");
  }
  return user_id;
}

td::Result<td::vector<td::int64>> Client::get_user_ids(const Query *query, size_t max_count, td::Slice field_name) {
  auto user_ids_str = query->arg(field_name);
  if (user_ids_str.empty()) {
    return td::Status::Error(400, "User identifiers are not specified");
  }

  auto r_value = json_decode(user_ids_str);
  if (r_value.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse " << field_name << " JSON object");
  }
  auto value = r_value.move_as_ok();
  if (value.type() != td::JsonValue::Type::Array) {
    return td::Status::Error(400, "Expected an Array of user identifiers");
  }
  if (value.get_array().size() > max_count) {
    return td::Status::Error(400, "Too many user identifiers specified");
  }

  td::vector<int64> user_ids;
  for (auto &user_id : value.get_array()) {
    td::Slice number;
    if (user_id.type() == td::JsonValue::Type::Number) {
      number = user_id.get_number();
    } else if (user_id.type() == td::JsonValue::Type::String) {
      number = user_id.get_string();
    } else {
      return td::Status::Error(400, "User identifier must be a Number");
    }
    auto parsed_user_id = td::to_integer_safe<int64>(number);
    if (parsed_user_id.is_error()) {
      return td::Status::Error(400, "Can't parse user identifier as a Number");
    }
    if (parsed_user_id.ok() <= 0) {
      return td::Status::Error(400, "Invalid user identifier specified");
    }
    user_ids.push_back(parsed_user_id.ok());
  }
  return std::move(user_ids);
}

td::Result<td::vector<td::string>> Client::get_strings(const Query *query, size_t max_count, td::Slice field_name) {
  auto strings_str = query->arg(field_name);
  if (strings_str.empty()) {
    return td::vector<td::string>();
  }

  auto r_value = json_decode(strings_str);
  if (r_value.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse " << field_name << " JSON object");
  }
  auto value = r_value.move_as_ok();
  if (value.type() != td::JsonValue::Type::Array) {
    return td::Status::Error(400, "Expected an Array of String");
  }
  if (value.get_array().size() > max_count) {
    return td::Status::Error(400, "Too many items specified");
  }

  td::vector<td::string> strings;
  for (auto &string : value.get_array()) {
    if (string.type() != td::JsonValue::Type::String) {
      return td::Status::Error(400, "Expected an Array of String");
    }
    strings.push_back(string.get_string().str());
  }
  return std::move(strings);
}

void Client::decrease_yet_unsent_message_count(int64 chat_id, int32 count) {
  auto count_it = yet_unsent_message_count_.find(chat_id);
  CHECK(count_it != yet_unsent_message_count_.end());
  CHECK(count_it->second >= count);
  count_it->second -= count;
  if (count_it->second == 0) {
    yet_unsent_message_count_.erase(count_it);
  }
}

td::int64 Client::extract_yet_unsent_message_query_id(int64 chat_id, int64 message_id) {
  auto yet_unsent_message_it = yet_unsent_messages_.find({chat_id, message_id});
  CHECK(yet_unsent_message_it != yet_unsent_messages_.end());

  auto query_id = yet_unsent_message_it->second.send_message_query_id;

  yet_unsent_messages_.erase(yet_unsent_message_it);

  decrease_yet_unsent_message_count(chat_id, 1);

  return query_id;
}

void Client::on_message_send_succeeded(object_ptr<td_api::message> &&message, int64 old_message_id) {
  const auto *message_info = add_message(std::move(message), true);
  int64 chat_id = message_info->chat_id;
  int64 new_message_id = message_info->id;
  CHECK(new_message_id > 0);

  message_info->is_content_changed = false;

  auto query_id = extract_yet_unsent_message_query_id(chat_id, old_message_id);
  auto &query = *pending_send_message_queries_[query_id];
  if (query.is_multisend) {
    if (query.query->method() == "forwardmessages" || query.query->method() == "copymessages") {
      query.messages.push_back(td::json_encode<td::string>(JsonMessageId(new_message_id)));
    } else {
      query.messages.push_back(td::json_encode<td::string>(JsonMessage(message_info, true, "sent message", this)));
    }
    query.awaited_message_count--;

    if (query.awaited_message_count == 0) {
      if (query.error == nullptr) {
        answer_query(JsonMessages(query.messages), std::move(query.query));
      } else {
        fail_query_with_error(std::move(query.query), std::move(query.error));
      }
      pending_send_message_queries_.erase(query_id);
    }
  } else {
    CHECK(query.awaited_message_count == 1);
    if (query.query->method() == "copymessage") {
      answer_query(JsonMessageId(new_message_id), std::move(query.query));
    } else {
      answer_query(JsonMessage(message_info, true, "sent message", this), std::move(query.query));
    }
    pending_send_message_queries_.erase(query_id);
  }
}

void Client::on_message_send_failed(int64 chat_id, int64 old_message_id, int64 new_message_id,
                                    object_ptr<td_api::error> &&error) {
  auto query_id = extract_yet_unsent_message_query_id(chat_id, old_message_id);
  auto &query = *pending_send_message_queries_[query_id];
  if (query.is_multisend) {
    if (query.error == nullptr || query.error->message_ == "Group send failed") {
      if (is_special_error_code(error->code_) || error->code_ == 406 || error->message_ == "Group send failed") {
        query.error = std::move(error);
      } else {
        auto pos = (query.total_message_count - query.awaited_message_count + 1);
        query.error = make_object<td_api::error>(error->code_, PSTRING() << "Failed to send message #" << pos
                                                                         << " with the error message \""
                                                                         << error->message_ << '"');
      }
    }
    query.awaited_message_count--;

    if (query.awaited_message_count == 0) {
      fail_query_with_error(std::move(query.query), std::move(query.error));
      pending_send_message_queries_.erase(query_id);
    }
  } else {
    CHECK(query.awaited_message_count == 1);
    fail_query_with_error(std::move(query.query), std::move(error));
    pending_send_message_queries_.erase(query_id);
  }

  if (new_message_id != 0 && !logging_out_ && !closing_) {
    send_request(make_object<td_api::deleteMessages>(chat_id, td::vector<int64>{new_message_id}, false),
                 td::make_unique<TdOnDeleteFailedToSendMessageCallback>(this, chat_id, new_message_id));
  }
}

void Client::on_story_send_succeeded(object_ptr<td_api::story> &&story, int64 old_story_id) {
  auto story_full_id = MessageFullId{story->poster_chat_id_, old_story_id};
  auto yet_unsent_story_it = yet_unsent_stories_.find(story_full_id);
  CHECK(yet_unsent_story_it != yet_unsent_stories_.end());
  auto query = std::move(yet_unsent_story_it->second.query);
  yet_unsent_stories_.erase(yet_unsent_story_it);
  answer_query(JsonStory(story->poster_chat_id_, story->id_, this), std::move(query));
}

void Client::on_story_send_failed(int64 chat_id, int64 story_id, object_ptr<td_api::error> &&error) {
  auto story_full_id = MessageFullId{chat_id, story_id};
  auto yet_unsent_story_it = yet_unsent_stories_.find(story_full_id);
  CHECK(yet_unsent_story_it != yet_unsent_stories_.end());
  auto query = std::move(yet_unsent_story_it->second.query);
  yet_unsent_stories_.erase(yet_unsent_story_it);
  fail_query_with_error(std::move(query), std::move(error));
}

void Client::on_cmd(PromisedQueryPtr query, bool force) {
  LOG(DEBUG) << "Process query " << *query;
  if (!td_client_.empty() && was_authorized_) {
    if (query->method() == "close") {
      auto retry_after = static_cast<int>(10 * 60 - (td::Time::now() - start_time_));
      if (retry_after > 0 && start_time_ > parameters_->start_time_ + 10 * 60) {
        return query->set_retry_after_error(retry_after);
      }
      need_close_ = true;
      return do_send_request(make_object<td_api::close>(), td::make_unique<TdOnOkQueryCallback>(std::move(query)));
    }
    if (query->method() == "logout") {
      clear_tqueue_ = true;
      return do_send_request(make_object<td_api::logOut>(), td::make_unique<TdOnOkQueryCallback>(std::move(query)));
    }
  }

  if (logging_out_ || closing_) {
    return fail_query_closing(std::move(query));
  }
  CHECK(was_authorized_);

  bot_user_ids_.unresolved_bot_usernames_.clear();

  auto method_it = methods_.find(query->method().str());
  if (method_it == methods_.end()) {
    return fail_query(404, "Not Found: method not found", std::move(query));
  }

  if (!query->files().empty() && !parameters_->local_mode_ && !force) {
    auto file_size = query->files_size();
    if (file_size > 100000) {
      auto &last_send_message_time = last_send_message_time_[file_size];
      auto now = td::Time::now();
      auto min_delay = td::clamp(static_cast<double>(file_size) * 1e-7, 0.2, 0.9);
      auto max_bucket_volume = 1.0;
      if (last_send_message_time > now + 5.0) {
        return fail_query_flood_limit_exceeded(std::move(query));
      }

      last_send_message_time = td::max(last_send_message_time + min_delay, now - max_bucket_volume);
      LOG(DEBUG) << "Query with files of size " << file_size << " can be processed in " << last_send_message_time - now
                 << " seconds";

      td::create_actor<td::SleepActor>(
          "DeleteLastSendMessageTimeSleepActor", last_send_message_time + min_delay - (now - max_bucket_volume),
          td::PromiseCreator::lambda([actor_id = actor_id(this), file_size,
                                      max_delay = max_bucket_volume + min_delay](td::Result<td::Unit>) mutable {
            send_closure(actor_id, &Client::delete_last_send_message_time, file_size, max_delay);
          }))
          .release();

      if (last_send_message_time > now) {
        td::create_actor<td::SleepActor>(
            "DoSendMessageSleepActor", last_send_message_time - now,
            td::PromiseCreator::lambda(
                [actor_id = actor_id(this), query = std::move(query)](td::Result<td::Unit>) mutable {
                  send_closure(actor_id, &Client::on_cmd, std::move(query), true);
                }))
            .release();
        return;
      }
    }
  }

  auto result = (this->*(method_it->second))(query);
  if (result.is_error()) {
    fail_query_with_error(std::move(query), result.code(), result.message());
  }
}

td::Status Client::process_get_me_query(PromisedQueryPtr &query) {
  answer_query(JsonUser(my_id_, this, true), std::move(query));
  return td::Status::OK();
}

td::Status Client::process_get_my_commands_query(PromisedQueryPtr &query) {
  TRY_RESULT(scope, get_bot_command_scope(query.get()));

  check_bot_command_scope(std::move(scope), std::move(query),
                          [this](object_ptr<td_api::BotCommandScope> &&scope, PromisedQueryPtr query) {
                            auto language_code = query->arg("language_code").str();
                            send_request(make_object<td_api::getCommands>(std::move(scope), language_code),
                                         td::make_unique<TdOnGetMyCommandsCallback>(std::move(query)));
                          });
  return td::Status::OK();
}

td::Status Client::process_set_my_commands_query(PromisedQueryPtr &query) {
  TRY_RESULT(bot_commands, get_bot_commands(query.get()));
  TRY_RESULT(scope, get_bot_command_scope(query.get()));

  check_bot_command_scope(
      std::move(scope), std::move(query),
      [this, bot_commands = std::move(bot_commands)](object_ptr<td_api::BotCommandScope> &&scope,
                                                     PromisedQueryPtr query) mutable {
        auto language_code = query->arg("language_code").str();
        send_request(make_object<td_api::setCommands>(std::move(scope), language_code, std::move(bot_commands)),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_delete_my_commands_query(PromisedQueryPtr &query) {
  TRY_RESULT(scope, get_bot_command_scope(query.get()));

  check_bot_command_scope(std::move(scope), std::move(query),
                          [this](object_ptr<td_api::BotCommandScope> &&scope, PromisedQueryPtr query) {
                            auto language_code = query->arg("language_code").str();
                            send_request(make_object<td_api::deleteCommands>(std::move(scope), language_code),
                                         td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                          });
  return td::Status::OK();
}

td::Status Client::process_get_my_default_administrator_rights_query(PromisedQueryPtr &query) {
  bool for_channels = to_bool(query->arg("for_channels"));
  send_request(make_object<td_api::getUserFullInfo>(my_id_),
               td::make_unique<TdOnGetMyDefaultAdministratorRightsCallback>(for_channels, std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_set_my_default_administrator_rights_query(PromisedQueryPtr &query) {
  bool for_channels = to_bool(query->arg("for_channels"));
  TRY_RESULT(rights, get_chat_administrator_rights(query.get()));

  if (for_channels) {
    send_request(make_object<td_api::setDefaultChannelAdministratorRights>(std::move(rights)),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  } else {
    send_request(make_object<td_api::setDefaultGroupAdministratorRights>(std::move(rights)),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  }
  return td::Status::OK();
}

td::Status Client::process_get_my_name_query(PromisedQueryPtr &query) {
  auto language_code = query->arg("language_code");
  send_request(make_object<td_api::getBotName>(my_id_, language_code.str()),
               td::make_unique<TdOnGetMyNameCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_set_my_name_query(PromisedQueryPtr &query) {
  auto language_code = query->arg("language_code");
  auto name = query->arg("name");
  send_request(make_object<td_api::setBotName>(my_id_, language_code.str(), name.str()),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_set_my_profile_photo_query(PromisedQueryPtr &query) {
  TRY_RESULT(photo, get_input_chat_photo(query.get()));
  send_request(make_object<td_api::setProfilePhoto>(std::move(photo), false),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_remove_my_profile_photo_query(PromisedQueryPtr &query) {
  send_request(make_object<td_api::deleteProfilePhoto>(0), td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_get_my_description_query(PromisedQueryPtr &query) {
  auto language_code = query->arg("language_code");
  send_request(make_object<td_api::getBotInfoDescription>(my_id_, language_code.str()),
               td::make_unique<TdOnGetMyDescriptionCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_set_my_description_query(PromisedQueryPtr &query) {
  auto language_code = query->arg("language_code");
  auto description = query->arg("description");
  send_request(make_object<td_api::setBotInfoDescription>(my_id_, language_code.str(), description.str()),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_get_my_short_description_query(PromisedQueryPtr &query) {
  auto language_code = query->arg("language_code");
  send_request(make_object<td_api::getBotInfoShortDescription>(my_id_, language_code.str()),
               td::make_unique<TdOnGetMyShortDescriptionCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_set_my_short_description_query(PromisedQueryPtr &query) {
  auto language_code = query->arg("language_code");
  auto short_description = query->arg("short_description");
  send_request(make_object<td_api::setBotInfoShortDescription>(my_id_, language_code.str(), short_description.str()),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_get_chat_menu_button_query(PromisedQueryPtr &query) {
  if (query->has_arg("chat_id")) {
    TRY_RESULT(user_id, get_user_id(query.get(), "chat_id"));
    check_user(user_id, std::move(query), [this, user_id](PromisedQueryPtr query) {
      send_request(make_object<td_api::getMenuButton>(user_id),
                   td::make_unique<TdOnGetMenuButtonCallback>(std::move(query)));
    });
  } else {
    send_request(make_object<td_api::getMenuButton>(0), td::make_unique<TdOnGetMenuButtonCallback>(std::move(query)));
  }
  return td::Status::OK();
}

td::Status Client::process_set_chat_menu_button_query(PromisedQueryPtr &query) {
  TRY_RESULT(menu_button, get_bot_menu_button(query.get()));
  if (query->has_arg("chat_id")) {
    TRY_RESULT(user_id, get_user_id(query.get(), "chat_id"));
    check_user(user_id, std::move(query),
               [this, user_id, menu_button = std::move(menu_button)](PromisedQueryPtr query) mutable {
                 send_request(make_object<td_api::setMenuButton>(user_id, std::move(menu_button)),
                              td::make_unique<TdOnOkQueryCallback>(std::move(query)));
               });
  } else {
    send_request(make_object<td_api::setMenuButton>(0, std::move(menu_button)),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  }
  return td::Status::OK();
}

td::Status Client::process_get_user_profile_photos_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  int32 offset = get_integer_arg(query.get(), "offset", 0, 0);
  int32 limit = get_integer_arg(query.get(), "limit", 100, 1, 100);

  check_user(user_id, std::move(query), [this, user_id, offset, limit](PromisedQueryPtr query) {
    send_request(make_object<td_api::getUserProfilePhotos>(user_id, offset, limit),
                 td::make_unique<TdOnGetUserProfilePhotosCallback>(this, std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_get_user_profile_audios_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  int32 offset = get_integer_arg(query.get(), "offset", 0, 0);
  int32 limit = get_integer_arg(query.get(), "limit", 100, 1, 100);

  check_user(user_id, std::move(query), [this, user_id, offset, limit](PromisedQueryPtr query) {
    send_request(make_object<td_api::getUserProfileAudios>(user_id, offset, limit),
                 td::make_unique<TdOnGetUserProfileAudiosCallback>(this, std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_send_message_query(PromisedQueryPtr &query) {
  auto r_chat_id = td::to_integer_safe<int64>(query->arg("chat_id"));
  if (r_chat_id.is_ok()) {
    // fast path
    auto it = yet_unsent_message_count_.find(r_chat_id.ok());
    if (it != yet_unsent_message_count_.end() && it->second >= MAX_CONCURRENTLY_SENT_CHAT_MESSAGES) {
      fail_query_flood_limit_exceeded(std::move(query));
      return td::Status::OK();
    }
  }

  TRY_RESULT(input_message_text, get_input_message_text(query.get()));
  do_send_message(std::move(input_message_text), std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_rich_message_query(PromisedQueryPtr &query) {
  auto r_chat_id = td::to_integer_safe<int64>(query->arg("chat_id"));
  if (r_chat_id.is_ok()) {
    // fast path
    auto it = yet_unsent_message_count_.find(r_chat_id.ok());
    if (it != yet_unsent_message_count_.end() && it->second >= MAX_CONCURRENTLY_SENT_CHAT_MESSAGES) {
      fail_query_flood_limit_exceeded(std::move(query));
      return td::Status::OK();
    }
  }

  TRY_RESULT(rich_message, get_input_rich_message(query.get()));
  do_send_message(make_object<td_api::inputMessageRichMessage>(std::move(rich_message), false), std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_animation_query(PromisedQueryPtr &query) {
  auto animation = get_input_file(query.get(), "animation");
  if (animation == nullptr) {
    return td::Status::Error(400, "There is no animation in the request");
  }
  auto thumbnail = get_input_thumbnail(query.get());
  int32 duration = get_integer_arg(query.get(), "duration", 0, 0, MAX_DURATION);
  int32 width = get_integer_arg(query.get(), "width", 0, 0, MAX_LENGTH);
  int32 height = get_integer_arg(query.get(), "height", 0, 0, MAX_LENGTH);
  TRY_RESULT(caption, get_caption(query.get()));
  auto show_caption_above_media = to_bool(query->arg("show_caption_above_media"));
  auto has_spoiler = to_bool(query->arg("has_spoiler"));
  do_send_message(make_object<td_api::inputMessageAnimation>(
                      make_object<td_api::inputAnimation>(std::move(animation), std::move(thumbnail),
                                                          td::vector<int32>(), duration, width, height),
                      std::move(caption), show_caption_above_media, has_spoiler),
                  std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_audio_query(PromisedQueryPtr &query) {
  auto audio = get_input_file(query.get(), "audio");
  if (audio == nullptr) {
    return td::Status::Error(400, "There is no audio in the request");
  }
  auto thumbnail = get_input_thumbnail(query.get());
  int32 duration = get_integer_arg(query.get(), "duration", 0, 0, MAX_DURATION);
  auto title = query->arg("title").str();
  auto performer = query->arg("performer").str();
  TRY_RESULT(caption, get_caption(query.get()));
  do_send_message(
      make_object<td_api::inputMessageAudio>(
          make_object<td_api::inputAudio>(std::move(audio), std::move(thumbnail), duration, title, performer),
          std::move(caption)),
      std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_dice_query(PromisedQueryPtr &query) {
  auto emoji = query->arg("emoji");
  do_send_message(make_object<td_api::inputMessageDice>(emoji.str(), false), std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_document_query(PromisedQueryPtr &query) {
  auto document = get_input_file(query.get(), "document");
  if (document == nullptr) {
    return td::Status::Error(400, "There is no document in the request");
  }
  auto thumbnail = get_input_thumbnail(query.get());
  TRY_RESULT(caption, get_caption(query.get()));
  bool disable_content_type_detection = to_bool(query->arg("disable_content_type_detection"));
  do_send_message(
      make_object<td_api::inputMessageDocument>(
          make_object<td_api::inputDocument>(std::move(document), std::move(thumbnail), disable_content_type_detection),
          std::move(caption)),
      std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_photo_query(PromisedQueryPtr &query) {
  auto photo = get_input_file(query.get(), "photo");
  if (photo == nullptr) {
    return td::Status::Error(400, "There is no photo in the request");
  }
  TRY_RESULT(caption, get_caption(query.get()));
  auto show_caption_above_media = to_bool(query->arg("show_caption_above_media"));
  auto has_spoiler = to_bool(query->arg("has_spoiler"));
  do_send_message(make_object<td_api::inputMessagePhoto>(
                      make_object<td_api::inputPhoto>(std::move(photo), nullptr, nullptr, td::vector<int32>(), 0, 0),
                      std::move(caption), show_caption_above_media, nullptr, has_spoiler),
                  std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_live_photo_query(PromisedQueryPtr &query) {
  auto live_photo = get_input_file(query.get(), "live_photo");
  if (live_photo == nullptr) {
    return td::Status::Error(400, "There is no live photo in the request");
  }
  auto photo = get_input_file(query.get(), "photo");
  if (photo == nullptr) {
    return td::Status::Error(400, "There is no photo in the request");
  }
  TRY_RESULT(caption, get_caption(query.get()));
  auto show_caption_above_media = to_bool(query->arg("show_caption_above_media"));
  auto has_spoiler = to_bool(query->arg("has_spoiler"));
  do_send_message(
      make_object<td_api::inputMessagePhoto>(
          make_object<td_api::inputPhoto>(std::move(photo), nullptr, std::move(live_photo), td::vector<int32>(), 0, 0),
          std::move(caption), show_caption_above_media, nullptr, has_spoiler),
      std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_sticker_query(PromisedQueryPtr &query) {
  auto sticker = get_input_file(query.get(), "sticker");
  if (sticker == nullptr) {
    return td::Status::Error(400, "There is no sticker in the request");
  }
  auto emoji = query->arg("emoji");
  do_send_message(make_object<td_api::inputMessageSticker>(
                      make_object<td_api::inputSticker>(std::move(sticker), nullptr, 0, 0), emoji.str()),
                  std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_video_query(PromisedQueryPtr &query) {
  auto video = get_input_file(query.get(), "video");
  if (video == nullptr) {
    return td::Status::Error(400, "There is no video in the request");
  }
  auto thumbnail = get_input_thumbnail(query.get());
  int32 duration = get_integer_arg(query.get(), "duration", 0, 0, MAX_DURATION);
  int32 width = get_integer_arg(query.get(), "width", 0, 0, MAX_LENGTH);
  int32 height = get_integer_arg(query.get(), "height", 0, 0, MAX_LENGTH);
  auto cover = get_input_file(query.get(), "cover");
  int32 start_timestamp = get_integer_arg(query.get(), "start_timestamp", 0, 0, MAX_DURATION);
  bool supports_streaming = to_bool(query->arg("supports_streaming"));
  TRY_RESULT(caption, get_caption(query.get()));
  auto show_caption_above_media = to_bool(query->arg("show_caption_above_media"));
  auto has_spoiler = to_bool(query->arg("has_spoiler"));
  do_send_message(
      make_object<td_api::inputMessageVideo>(
          make_object<td_api::inputVideo>(std::move(video), std::move(thumbnail), std::move(cover), start_timestamp,
                                          td::vector<int32>(), duration, width, height, supports_streaming),
          std::move(caption), show_caption_above_media, nullptr, has_spoiler),
      std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_video_note_query(PromisedQueryPtr &query) {
  auto video_note = get_input_file(query.get(), "video_note");
  if (video_note == nullptr) {
    return td::Status::Error(400, "There is no video note in the request");
  }
  auto thumbnail = get_input_thumbnail(query.get());
  int32 duration = get_integer_arg(query.get(), "duration", 0, 0, MAX_DURATION);
  int32 length = get_integer_arg(query.get(), "length", 0, 0, MAX_LENGTH);
  do_send_message(
      make_object<td_api::inputMessageVideoNote>(
          make_object<td_api::inputVideoNote>(std::move(video_note), std::move(thumbnail), duration, length), nullptr),
      std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_voice_query(PromisedQueryPtr &query) {
  auto voice_note = get_input_file(query.get(), "voice");
  if (voice_note == nullptr) {
    return td::Status::Error(400, "There is no voice in the request");
  }
  int32 duration = get_integer_arg(query.get(), "duration", 0, 0, MAX_DURATION);
  TRY_RESULT(caption, get_caption(query.get()));
  do_send_message(
      make_object<td_api::inputMessageVoiceNote>(
          make_object<td_api::inputVoiceNote>(std::move(voice_note), duration, ""), std::move(caption), nullptr),
      std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_paid_media_query(PromisedQueryPtr &query) {
  int32 star_count = get_integer_arg(query.get(), "star_count", 0, 0, 1000000000);
  TRY_RESULT(paid_media, get_paid_media(query.get(), "media"));
  TRY_RESULT(caption, get_caption(query.get()));
  auto payload = query->arg("payload").str();
  auto show_caption_above_media = to_bool(query->arg("show_caption_above_media"));
  do_send_message(make_object<td_api::inputMessagePaidMedia>(star_count, std::move(paid_media), std::move(caption),
                                                             show_caption_above_media, payload),
                  std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_game_query(PromisedQueryPtr &query) {
  TRY_RESULT(game_short_name, get_required_string_arg(query.get(), "game_short_name"));
  do_send_message(make_object<td_api::inputMessageGame>(my_id_, game_short_name.str()), std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_invoice_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_message_invoice, get_input_message_invoice(query.get()));
  do_send_message(std::move(input_message_invoice), std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_location_query(PromisedQueryPtr &query) {
  TRY_RESULT(location, get_location(query.get()));
  int32 live_period = get_integer_arg(query.get(), "live_period", 0);
  int32 heading = get_integer_arg(query.get(), "heading", 0);
  int32 proximity_alert_radius = get_integer_arg(query.get(), "proximity_alert_radius", 0);

  if (live_period != 0) {
    do_send_message(make_object<td_api::inputMessageLiveLocation>(make_object<td_api::liveLocation>(
                        std::move(location), live_period, heading, proximity_alert_radius)),
                    std::move(query));
  } else {
    do_send_message(make_object<td_api::inputMessageLocation>(std::move(location)), std::move(query));
  }
  return td::Status::OK();
}

td::Status Client::process_send_venue_query(PromisedQueryPtr &query) {
  TRY_RESULT(location, get_location(query.get()));

  auto title = query->arg("title");
  auto address = query->arg("address");
  td::string provider;
  td::string venue_id;
  td::string venue_type;

  auto google_place_id = query->arg("google_place_id");
  auto google_place_type = query->arg("google_place_type");
  if (!google_place_id.empty() || !google_place_type.empty()) {
    provider = "gplaces";
    venue_id = google_place_id.str();
    venue_type = google_place_type.str();
  }
  auto foursquare_id = query->arg("foursquare_id");
  auto foursquare_type = query->arg("foursquare_type");
  if (!foursquare_id.empty() || !foursquare_type.empty()) {
    provider = "foursquare";
    venue_id = foursquare_id.str();
    venue_type = foursquare_type.str();
  }

  do_send_message(make_object<td_api::inputMessageVenue>(make_object<td_api::venue>(
                      std::move(location), title.str(), address.str(), provider, venue_id, venue_type)),
                  std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_contact_query(PromisedQueryPtr &query) {
  TRY_RESULT(phone_number, get_required_string_arg(query.get(), "phone_number"));
  TRY_RESULT(first_name, get_required_string_arg(query.get(), "first_name"));
  auto last_name = query->arg("last_name");
  auto vcard = query->arg("vcard");
  do_send_message(make_object<td_api::inputMessageContact>(make_object<td_api::contact>(
                      phone_number.str(), first_name.str(), last_name.str(), vcard.str(), 0)),
                  std::move(query));
  return td::Status::OK();
}

td::Status Client::process_send_poll_query(PromisedQueryPtr &query) {
  TRY_RESULT(question, get_formatted_text(query->arg("question").str(), query->arg("question_parse_mode").str(),
                                          get_input_entities(query.get(), "question_entities")));
  TRY_RESULT(options, get_input_poll_options(query.get()));
  TRY_RESULT(description,
             get_formatted_text(query->arg("description").str(), query->arg("description_parse_mode").str(),
                                get_input_entities(query.get(), "description_entities")));
  bool is_anonymous = true;
  if (query->has_arg("is_anonymous")) {
    is_anonymous = to_bool(query->arg("is_anonymous"));
  }

  auto allows_multiple_answers = to_bool(query->arg("allows_multiple_answers"));
  object_ptr<td_api::InputPollType> poll_type;
  auto type = query->arg("type");
  if (type == "quiz") {
    TRY_RESULT(explanation,
               get_formatted_text(query->arg("explanation").str(), query->arg("explanation_parse_mode").str(),
                                  get_input_entities(query.get(), "explanation_entities")));
    TRY_RESULT(explanation_media, get_input_poll_media(query.get(), "explanation_media"));
    td::vector<int32> correct_option_ids;
    if (query->has_arg("correct_option_ids")) {
      auto r_value = json_decode(query->arg("correct_option_ids"));
      if (r_value.is_error()) {
        return td::Status::Error(400, "Can't parse correct option identifiers JSON object");
      }
      auto value = r_value.move_as_ok();
      if (value.type() != td::JsonValue::Type::Array) {
        return td::Status::Error(400, "Expected an Array of correct option identifiers");
      }

      for (auto &correct_option_id : value.get_array()) {
        if (correct_option_id.type() != td::JsonValue::Type::Number) {
          return td::Status::Error(400, "Correct option identifier must be of type Number");
        }
        auto parsed_id = td::to_integer_safe<int32>(correct_option_id.get_number());
        if (parsed_id.is_error()) {
          return td::Status::Error(400, "Invalid correct option identifier specified");
        }
        correct_option_ids.push_back(parsed_id.ok());
      }
    } else if (query->has_arg("correct_option_id")) {
      correct_option_ids.push_back(get_integer_arg(query.get(), "correct_option_id", -1));
    }

    poll_type = make_object<td_api::inputPollTypeQuiz>(std::move(correct_option_ids), std::move(explanation),
                                                       std::move(explanation_media));
  } else if (type.empty() || type == "regular") {
    poll_type = make_object<td_api::inputPollTypeRegular>(to_bool(query->arg("allow_adding_options")));
  } else {
    return td::Status::Error(400, "Unsupported poll type specified");
  }
  bool allows_revoting = type != "quiz";
  if (query->has_arg("allows_revoting")) {
    allows_revoting = to_bool(query->arg("allows_revoting"));
  }
  int32 open_period = get_integer_arg(query.get(), "open_period", 0, 0, 2628000);
  int32 close_date = get_integer_arg(query.get(), "close_date", 0);
  auto is_closed = to_bool(query->arg("is_closed"));
  auto shuffle_options = to_bool(query->arg("shuffle_options"));
  auto hide_results_until_closes = to_bool(query->arg("hide_results_until_closes"));
  auto members_only = to_bool(query->arg("members_only"));
  TRY_RESULT(country_codes, get_strings(query.get(), 12, "country_codes"));
  TRY_RESULT(media, get_input_poll_media(query.get(), "media"));
  do_send_message(make_object<td_api::inputMessagePoll>(
                      std::move(question), std::move(options), std::move(description), std::move(media), is_anonymous,
                      allows_multiple_answers, allows_revoting, members_only, std::move(country_codes), shuffle_options,
                      hide_results_until_closes, std::move(poll_type), open_period, close_date, is_closed),
                  std::move(query));
  return td::Status::OK();
}

td::Status Client::process_stop_poll_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id");
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get(), bot_user_ids_));

  resolve_reply_markup_bot_usernames(
      std::move(reply_markup), std::move(query),
      [this, business_connection_id = business_connection_id.str(), chat_id_str = chat_id.str(), message_id](
          object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) {
        if (!business_connection_id.empty()) {
          return check_business_connection_chat_id(
              business_connection_id, chat_id_str, std::move(query),
              [this, message_id, reply_markup = std::move(reply_markup)](
                  const BusinessConnection *business_connection, int64 chat_id, PromisedQueryPtr query) mutable {
                send_request(
                    make_object<td_api::stopBusinessPoll>(business_connection->id_, chat_id, message_id,
                                                          std::move(reply_markup)),
                    td::make_unique<TdOnStopBusinessPollCallback>(this, business_connection->id_, std::move(query)));
              });
        }

        check_message(chat_id_str, message_id, false, AccessRights::Edit, "message with poll to stop", std::move(query),
                      [this, reply_markup = std::move(reply_markup)](int64 chat_id, int64 message_id,
                                                                     PromisedQueryPtr query) mutable {
                        send_request(
                            make_object<td_api::stopPoll>(chat_id, message_id, std::move(reply_markup)),
                            td::make_unique<TdOnStopPollCallback>(this, chat_id, message_id, std::move(query)));
                      });
      });
  return td::Status::OK();
}

td::Status Client::process_send_checklist_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_checklist, get_input_checklist(query.get(), "checklist"));
  do_send_message(make_object<td_api::inputMessageChecklist>(std::move(input_checklist)), std::move(query));
  return td::Status::OK();
}

td::Status Client::process_copy_message_query(PromisedQueryPtr &query) {
  TRY_RESULT(from_chat_id, get_required_string_arg(query.get(), "from_chat_id"));
  auto message_id = get_message_id(query.get());
  bool replace_caption = query->has_arg("caption");
  object_ptr<td_api::formattedText> caption;
  if (replace_caption) {
    TRY_RESULT_ASSIGN(caption, get_caption(query.get()));
  }
  bool replace_video_start_timestamp = query->has_arg("video_start_timestamp");
  int32 new_video_start_timestamp = get_integer_arg(query.get(), "video_start_timestamp", 0);
  auto show_caption_above_media = to_bool(query->arg("show_caption_above_media"));
  auto options =
      make_object<td_api::messageCopyOptions>(true, replace_caption, std::move(caption), show_caption_above_media);

  check_message(from_chat_id, message_id, false, AccessRights::Read, "message to copy", std::move(query),
                [this, replace_video_start_timestamp, new_video_start_timestamp, options = std::move(options)](
                    int64 from_chat_id, int64 message_id, PromisedQueryPtr query) mutable {
                  do_send_message(make_object<td_api::inputMessageForwarded>(
                                      from_chat_id, message_id, false, replace_video_start_timestamp,
                                      new_video_start_timestamp, std::move(options)),
                                  std::move(query));
                });
  return td::Status::OK();
}

td::Status Client::process_copy_messages_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto forum_topic_id = get_forum_topic_id(query.get(), "message_thread_id");
  TRY_RESULT(from_chat_id, get_required_string_arg(query.get(), "from_chat_id"));
  TRY_RESULT(message_ids, get_message_ids(query.get(), 100));
  if (message_ids.empty()) {
    return td::Status::Error(400, "Message identifiers are not specified");
  }
  auto disable_notification = to_bool(query->arg("disable_notification"));
  auto protect_content = to_bool(query->arg("protect_content"));
  auto effect_id = td::to_integer<int64>(query->arg("message_effect_id"));
  auto direct_messages_topic_id = td::to_integer<int64>(query->arg("direct_messages_topic_id"));
  TRY_RESULT(input_suggested_post_info, get_input_suggested_post_info(query.get()));
  auto remove_caption = to_bool(query->arg("remove_caption"));

  auto send_options = get_message_send_options(disable_notification, protect_content, false, effect_id,
                                               std::move(input_suggested_post_info));
  auto on_success = [this, from_chat_id = from_chat_id.str(), message_ids = std::move(message_ids),
                     send_options = std::move(send_options),
                     remove_caption](int64 chat_id, object_ptr<td_api::MessageTopic> topic_id, CheckedReplyParameters,
                                     PromisedQueryPtr query) mutable {
    auto it = yet_unsent_message_count_.find(chat_id);
    if (it != yet_unsent_message_count_.end() && it->second >= MAX_CONCURRENTLY_SENT_CHAT_MESSAGES) {
      return fail_query_flood_limit_exceeded(std::move(query));
    }

    check_messages(
        from_chat_id, std::move(message_ids), true, AccessRights::Read, "message to forward", std::move(query),
        [this, chat_id, topic_id = std::move(topic_id), send_options = std::move(send_options), remove_caption](
            int64 from_chat_id, td::vector<int64> message_ids, PromisedQueryPtr query) mutable {
          auto &count = yet_unsent_message_count_[chat_id];
          if (count >= MAX_CONCURRENTLY_SENT_CHAT_MESSAGES) {
            return fail_query_flood_limit_exceeded(std::move(query));
          }

          auto message_count = message_ids.size();
          count += static_cast<int32>(message_count);

          send_request(
              make_object<td_api::forwardMessages>(chat_id, std::move(topic_id), from_chat_id, std::move(message_ids),
                                                   std::move(send_options), true, remove_caption),
              td::make_unique<TdOnForwardMessagesCallback>(this, chat_id, message_count, std::move(query)));
        });
  };
  check_reply_parameters(chat_id, InputReplyParameters(), forum_topic_id, direct_messages_topic_id, std::move(query),
                         std::move(on_success));

  return td::Status::OK();
}

td::Status Client::process_forward_message_query(PromisedQueryPtr &query) {
  TRY_RESULT(from_chat_id, get_required_string_arg(query.get(), "from_chat_id"));
  auto message_id = get_message_id(query.get());
  bool replace_video_start_timestamp = query->has_arg("video_start_timestamp");
  int32 new_video_start_timestamp = get_integer_arg(query.get(), "video_start_timestamp", 0);

  check_message(from_chat_id, message_id, false, AccessRights::Read, "message to forward", std::move(query),
                [this, replace_video_start_timestamp, new_video_start_timestamp](int64 from_chat_id, int64 message_id,
                                                                                 PromisedQueryPtr query) {
                  do_send_message(make_object<td_api::inputMessageForwarded>(from_chat_id, message_id, false,
                                                                             replace_video_start_timestamp,
                                                                             new_video_start_timestamp, nullptr),
                                  std::move(query));
                });
  return td::Status::OK();
}

td::Status Client::process_forward_messages_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto forum_topic_id = get_forum_topic_id(query.get(), "message_thread_id");
  TRY_RESULT(from_chat_id, get_required_string_arg(query.get(), "from_chat_id"));
  TRY_RESULT(message_ids, get_message_ids(query.get(), 100));
  if (message_ids.empty()) {
    return td::Status::Error(400, "Message identifiers are not specified");
  }
  auto disable_notification = to_bool(query->arg("disable_notification"));
  auto protect_content = to_bool(query->arg("protect_content"));
  auto effect_id = td::to_integer<int64>(query->arg("message_effect_id"));
  auto direct_messages_topic_id = td::to_integer<int64>(query->arg("direct_messages_topic_id"));
  TRY_RESULT(input_suggested_post_info, get_input_suggested_post_info(query.get()));

  auto send_options = get_message_send_options(disable_notification, protect_content, false, effect_id,
                                               std::move(input_suggested_post_info));
  auto on_success = [this, from_chat_id = from_chat_id.str(), message_ids = std::move(message_ids),
                     send_options = std::move(send_options)](int64 chat_id, object_ptr<td_api::MessageTopic> topic_id,
                                                             CheckedReplyParameters, PromisedQueryPtr query) mutable {
    auto it = yet_unsent_message_count_.find(chat_id);
    if (it != yet_unsent_message_count_.end() && it->second >= MAX_CONCURRENTLY_SENT_CHAT_MESSAGES) {
      return fail_query_flood_limit_exceeded(std::move(query));
    }

    check_messages(
        from_chat_id, std::move(message_ids), true, AccessRights::Read, "message to forward", std::move(query),
        [this, chat_id, topic_id = std::move(topic_id), send_options = std::move(send_options)](
            int64 from_chat_id, td::vector<int64> message_ids, PromisedQueryPtr query) mutable {
          auto &count = yet_unsent_message_count_[chat_id];
          if (count >= MAX_CONCURRENTLY_SENT_CHAT_MESSAGES) {
            return fail_query_flood_limit_exceeded(std::move(query));
          }

          auto message_count = message_ids.size();
          count += static_cast<int32>(message_count);

          send_request(
              make_object<td_api::forwardMessages>(chat_id, std::move(topic_id), from_chat_id, std::move(message_ids),
                                                   std::move(send_options), false, false),
              td::make_unique<TdOnForwardMessagesCallback>(this, chat_id, message_count, std::move(query)));
        });
  };
  check_reply_parameters(chat_id, InputReplyParameters(), forum_topic_id, direct_messages_topic_id, std::move(query),
                         std::move(on_success));

  return td::Status::OK();
}

td::Status Client::process_send_media_group_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto forum_topic_id = get_forum_topic_id(query.get(), "message_thread_id");
  TRY_RESULT(reply_parameters, get_reply_parameters(query.get()));
  auto business_connection_id = query->arg("business_connection_id");
  auto disable_notification = to_bool(query->arg("disable_notification"));
  auto protect_content = to_bool(query->arg("protect_content"));
  auto allow_paid_broadcast = to_bool(query->arg("allow_paid_broadcast"));
  auto effect_id = td::to_integer<int64>(query->arg("message_effect_id"));
  auto direct_messages_topic_id = td::to_integer<int64>(query->arg("direct_messages_topic_id"));
  TRY_RESULT(input_suggested_post_info, get_input_suggested_post_info(query.get()));
  // TRY_RESULT(reply_markup, get_reply_markup(query.get(), bot_user_ids_));
  auto reply_markup = nullptr;
  TRY_RESULT(input_message_contents, get_input_message_contents(query.get(), "media"));

  auto send_options = get_message_send_options(disable_notification, protect_content, allow_paid_broadcast, effect_id,
                                               std::move(input_suggested_post_info));
  resolve_reply_markup_bot_usernames(
      std::move(reply_markup), std::move(query),
      [this, chat_id_str = chat_id.str(), forum_topic_id, direct_messages_topic_id,
       business_connection_id = business_connection_id.str(), reply_parameters = std::move(reply_parameters),
       disable_notification, protect_content, allow_paid_broadcast, effect_id, send_options = std::move(send_options),
       input_message_contents = std::move(input_message_contents)](object_ptr<td_api::ReplyMarkup> reply_markup,
                                                                   PromisedQueryPtr query) mutable {
        if (!business_connection_id.empty()) {
          return check_business_connection_chat_id(
              business_connection_id, chat_id_str, std::move(query),
              [this, reply_parameters = std::move(reply_parameters), disable_notification, protect_content, effect_id,
               input_message_contents = std::move(input_message_contents), reply_markup = std::move(reply_markup)](
                  const BusinessConnection *business_connection, int64 chat_id, PromisedQueryPtr query) mutable {
                send_request(make_object<td_api::sendBusinessMessageAlbum>(
                                 business_connection->id_, chat_id,
                                 get_input_message_reply_to_unchecked(std::move(reply_parameters)),
                                 disable_notification, protect_content, effect_id, std::move(input_message_contents)),
                             td::make_unique<TdOnSendBusinessMessageAlbumCallback>(this, business_connection->id_,
                                                                                   std::move(query)));
              });
        }

        auto on_success = [this, send_options = std::move(send_options),
                           input_message_contents = std::move(input_message_contents),
                           reply_markup = std::move(reply_markup)](
                              int64 chat_id, object_ptr<td_api::MessageTopic> topic_id,
                              CheckedReplyParameters reply_parameters, PromisedQueryPtr query) mutable {
          auto &count = yet_unsent_message_count_[chat_id];
          if (count >= MAX_CONCURRENTLY_SENT_CHAT_MESSAGES) {
            return fail_query_flood_limit_exceeded(std::move(query));
          }
          auto message_count = input_message_contents.size();
          count += static_cast<int32>(message_count);

          send_request(make_object<td_api::sendMessageAlbum>(
                           chat_id, std::move(topic_id), get_input_message_reply_to(std::move(reply_parameters)),
                           std::move(send_options), std::move(input_message_contents)),
                       td::make_unique<TdOnSendMessageAlbumCallback>(this, chat_id, message_count, std::move(query)));
        };
        check_reply_parameters(chat_id_str, std::move(reply_parameters), forum_topic_id, direct_messages_topic_id,
                               std::move(query), std::move(on_success), allow_paid_broadcast);
      });
  return td::Status::OK();
}

td::Status Client::process_send_chat_action_query(PromisedQueryPtr &query) {
  auto chat_id_str = query->arg("chat_id");
  auto forum_topic_id = get_forum_topic_id(query.get(), "message_thread_id");
  auto business_connection_id = query->arg("business_connection_id").str();
  object_ptr<td_api::ChatAction> action = get_chat_action(query.get());
  if (action == nullptr) {
    return td::Status::Error(400, "Wrong parameter action in request");
  }
  if (!business_connection_id.empty()) {
    check_business_connection_chat_id(
        business_connection_id, chat_id_str.str(), std::move(query),
        [this, action = std::move(action)](const BusinessConnection *business_connection, int64 chat_id,
                                           PromisedQueryPtr query) mutable {
          send_request(
              make_object<td_api::sendChatAction>(chat_id, nullptr, business_connection->id_, std::move(action)),
              td::make_unique<TdOnOkQueryCallback>(std::move(query)));
        });
    return td::Status::OK();
  }

  check_chat(chat_id_str, AccessRights::Write, std::move(query),
             [this, forum_topic_id, action = std::move(action)](int64 chat_id, PromisedQueryPtr query) mutable {
               send_request(
                   make_object<td_api::sendChatAction>(
                       chat_id, forum_topic_id != 0 ? make_object<td_api::messageTopicForum>(forum_topic_id) : nullptr,
                       td::string(), std::move(action)),
                   td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_send_message_draft_query(PromisedQueryPtr &query) {
  auto chat_id_str = query->arg("chat_id");
  auto forum_topic_id = get_forum_topic_id(query.get(), "message_thread_id");
  auto draft_id = td::to_integer<int64>(query->arg("draft_id"));
  auto can_stop = to_bool(query->arg("can_stop"));
  auto keep_on_stop = to_bool(query->arg("keep_on_stop"));
  TRY_RESULT(text, get_formatted_text(query->arg("text").str(), query->arg("parse_mode").str(),
                                      get_input_entities(query.get(), "entities")));

  check_chat(chat_id_str, AccessRights::Write, std::move(query),
             [this, forum_topic_id, draft_id, can_stop, keep_on_stop, text = std::move(text)](
                 int64 chat_id, PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::sendTextMessageDraft>(chat_id, forum_topic_id, draft_id, can_stop,
                                                                      keep_on_stop, std::move(text)),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_send_rich_message_draft_query(PromisedQueryPtr &query) {
  auto chat_id_str = query->arg("chat_id");
  auto forum_topic_id = get_forum_topic_id(query.get(), "message_thread_id");
  auto draft_id = td::to_integer<int64>(query->arg("draft_id"));
  auto can_stop = to_bool(query->arg("can_stop"));
  auto keep_on_stop = to_bool(query->arg("keep_on_stop"));
  TRY_RESULT(rich_message, get_input_rich_message(query.get()));

  check_chat(chat_id_str, AccessRights::Write, std::move(query),
             [this, forum_topic_id, draft_id, can_stop, keep_on_stop, rich_message = std::move(rich_message)](
                 int64 chat_id, PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::sendRichMessageDraft>(chat_id, forum_topic_id, draft_id, can_stop,
                                                                      keep_on_stop, std::move(rich_message)),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_set_message_reaction_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  auto is_big = to_bool(query->arg("is_big"));
  TRY_RESULT(reaction_types, get_reaction_types(query.get()));

  check_message(chat_id, message_id, false, AccessRights::Read, "message to react", std::move(query),
                [this, reaction_types = std::move(reaction_types), is_big](int64 chat_id, int64 message_id,
                                                                           PromisedQueryPtr query) mutable {
                  send_request(
                      make_object<td_api::setMessageReactions>(chat_id, message_id, std::move(reaction_types), is_big),
                      td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                });
  return td::Status::OK();
}

td::Status Client::process_edit_message_text_query(PromisedQueryPtr &query) {
  object_ptr<td_api::InputMessageContent> input_message_text;
  if (query->has_arg("rich_message")) {
    TRY_RESULT(input_rich_message, get_input_rich_message(query.get()));
    input_message_text = make_object<td_api::inputMessageRichMessage>(std::move(input_rich_message), false);
  } else {
    TRY_RESULT_ASSIGN(input_message_text, get_input_message_text(query.get()));
  }
  auto business_connection_id = query->arg("business_connection_id");
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get(), bot_user_ids_));

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, inline_message_id = inline_message_id.str(), input_message_text = std::move(input_message_text)](
            object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          send_request(make_object<td_api::editInlineMessageText>(inline_message_id, std::move(reply_markup),
                                                                  std::move(input_message_text)),
                       td::make_unique<TdOnEditInlineMessageCallback>(std::move(query)));
        });
  } else {
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, business_connection_id = business_connection_id.str(), chat_id_str = chat_id.str(), message_id,
         input_message_text = std::move(input_message_text)](object_ptr<td_api::ReplyMarkup> reply_markup,
                                                             PromisedQueryPtr query) mutable {
          if (!business_connection_id.empty()) {
            return check_business_connection_chat_id(
                business_connection_id, chat_id_str, std::move(query),
                [this, message_id, input_message_text = std::move(input_message_text),
                 reply_markup = std::move(reply_markup)](const BusinessConnection *business_connection, int64 chat_id,
                                                         PromisedQueryPtr query) mutable {
                  send_request(make_object<td_api::editBusinessMessageText>(business_connection->id_, chat_id,
                                                                            message_id, std::move(reply_markup),
                                                                            std::move(input_message_text)),
                               td::make_unique<TdOnReturnBusinessMessageCallback>(this, business_connection->id_,
                                                                                  std::move(query)));
                });
          }

          check_message(
              chat_id_str, message_id, false, AccessRights::Edit, "message to edit", std::move(query),
              [this, input_message_text = std::move(input_message_text), reply_markup = std::move(reply_markup)](
                  int64 chat_id, int64 message_id, PromisedQueryPtr query) mutable {
                send_request(make_object<td_api::editMessageText>(chat_id, message_id, std::move(reply_markup),
                                                                  std::move(input_message_text)),
                             td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
              });
        });
  }
  return td::Status::OK();
}

td::Status Client::process_edit_message_live_location_query(PromisedQueryPtr &query) {
  object_ptr<td_api::location> location = nullptr;
  int32 live_period = get_integer_arg(query.get(), "live_period", 0);
  int32 heading = get_integer_arg(query.get(), "heading", 0);
  int32 proximity_alert_radius = get_integer_arg(query.get(), "proximity_alert_radius", 0);
  if (query->method() == "editmessagelivelocation") {
    TRY_RESULT_ASSIGN(location, get_location(query.get()));
  }
  auto business_connection_id = query->arg("business_connection_id");
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get(), bot_user_ids_));
  auto live_location =
      make_object<td_api::liveLocation>(std::move(location), live_period, heading, proximity_alert_radius);

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, inline_message_id = inline_message_id.str(), live_location = std::move(live_location)](
            object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          send_request(make_object<td_api::editInlineMessageLiveLocation>(inline_message_id, std::move(reply_markup),
                                                                          std::move(live_location)),
                       td::make_unique<TdOnEditInlineMessageCallback>(std::move(query)));
        });
  } else {
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, business_connection_id = business_connection_id.str(), chat_id_str = chat_id.str(), message_id,
         live_location = std::move(live_location)](object_ptr<td_api::ReplyMarkup> reply_markup,
                                                   PromisedQueryPtr query) mutable {
          if (!business_connection_id.empty()) {
            return check_business_connection_chat_id(
                business_connection_id, chat_id_str, std::move(query),
                [this, message_id, live_location = std::move(live_location), reply_markup = std::move(reply_markup)](
                    const BusinessConnection *business_connection, int64 chat_id, PromisedQueryPtr query) mutable {
                  send_request(make_object<td_api::editBusinessMessageLiveLocation>(business_connection->id_, chat_id,
                                                                                    message_id, std::move(reply_markup),
                                                                                    std::move(live_location)),
                               td::make_unique<TdOnReturnBusinessMessageCallback>(this, business_connection->id_,
                                                                                  std::move(query)));
                });
          }

          check_message(chat_id_str, message_id, false, AccessRights::Edit, "message to edit", std::move(query),
                        [this, live_location = std::move(live_location), reply_markup = std::move(reply_markup)](
                            int64 chat_id, int64 message_id, PromisedQueryPtr query) mutable {
                          send_request(make_object<td_api::editMessageLiveLocation>(
                                           chat_id, message_id, std::move(reply_markup), std::move(live_location)),
                                       td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
                        });
        });
  }
  return td::Status::OK();
}

td::Status Client::process_edit_message_media_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id");
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get(), bot_user_ids_));
  TRY_RESULT(input_media, get_input_media(query.get(), "media"));

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, inline_message_id = inline_message_id.str(), input_message_content = std::move(input_media)](
            object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          send_request(make_object<td_api::editInlineMessageMedia>(inline_message_id, std::move(reply_markup),
                                                                   std::move(input_message_content)),
                       td::make_unique<TdOnEditInlineMessageCallback>(std::move(query)));
        });
  } else {
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, business_connection_id = business_connection_id.str(), chat_id_str = chat_id.str(), message_id,
         input_message_content = std::move(input_media)](object_ptr<td_api::ReplyMarkup> reply_markup,
                                                         PromisedQueryPtr query) mutable {
          if (!business_connection_id.empty()) {
            return check_business_connection_chat_id(
                business_connection_id, chat_id_str, std::move(query),
                [this, message_id, input_message_content = std::move(input_message_content),
                 reply_markup = std::move(reply_markup)](const BusinessConnection *business_connection, int64 chat_id,
                                                         PromisedQueryPtr query) mutable {
                  send_request(make_object<td_api::editBusinessMessageMedia>(business_connection->id_, chat_id,
                                                                             message_id, std::move(reply_markup),
                                                                             std::move(input_message_content)),
                               td::make_unique<TdOnReturnBusinessMessageCallback>(this, business_connection->id_,
                                                                                  std::move(query)));
                });
          }

          check_message(
              chat_id_str, message_id, false, AccessRights::Edit, "message to edit", std::move(query),
              [this, reply_markup = std::move(reply_markup), input_message_content = std::move(input_message_content)](
                  int64 chat_id, int64 message_id, PromisedQueryPtr query) mutable {
                send_request(make_object<td_api::editMessageMedia>(chat_id, message_id, std::move(reply_markup),
                                                                   std::move(input_message_content)),
                             td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
              });
        });
  }
  return td::Status::OK();
}

td::Status Client::process_edit_message_caption_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id");
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get(), bot_user_ids_));
  TRY_RESULT(caption, get_caption(query.get()));
  auto show_caption_above_media = to_bool(query->arg("show_caption_above_media"));

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, inline_message_id = inline_message_id.str(), caption = std::move(caption), show_caption_above_media](
            object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          send_request(make_object<td_api::editInlineMessageCaption>(inline_message_id, std::move(reply_markup),
                                                                     std::move(caption), show_caption_above_media),
                       td::make_unique<TdOnEditInlineMessageCallback>(std::move(query)));
        });
  } else {
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, business_connection_id = business_connection_id.str(), chat_id_str = chat_id.str(), message_id,
         caption = std::move(caption),
         show_caption_above_media](object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
          if (!business_connection_id.empty()) {
            return check_business_connection_chat_id(
                business_connection_id, chat_id_str, std::move(query),
                [this, message_id, reply_markup = std::move(reply_markup), caption = std::move(caption),
                 show_caption_above_media](const BusinessConnection *business_connection, int64 chat_id,
                                           PromisedQueryPtr query) mutable {
                  send_request(make_object<td_api::editBusinessMessageCaption>(
                                   business_connection->id_, chat_id, message_id, std::move(reply_markup),
                                   std::move(caption), show_caption_above_media),
                               td::make_unique<TdOnReturnBusinessMessageCallback>(this, business_connection->id_,
                                                                                  std::move(query)));
                });
          }

          check_message(chat_id_str, message_id, false, AccessRights::Edit, "message to edit", std::move(query),
                        [this, reply_markup = std::move(reply_markup), caption = std::move(caption),
                         show_caption_above_media](int64 chat_id, int64 message_id, PromisedQueryPtr query) mutable {
                          send_request(
                              make_object<td_api::editMessageCaption>(chat_id, message_id, std::move(reply_markup),
                                                                      std::move(caption), show_caption_above_media),
                              td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
                        });
        });
  }
  return td::Status::OK();
}

td::Status Client::process_edit_message_checklist_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id");
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get(), bot_user_ids_));
  TRY_RESULT(input_checklist, get_input_checklist(query.get(), "checklist"));

  resolve_reply_markup_bot_usernames(
      std::move(reply_markup), std::move(query),
      [this, business_connection_id = business_connection_id.str(), chat_id_str = chat_id.str(), message_id,
       input_checklist = std::move(input_checklist)](object_ptr<td_api::ReplyMarkup> reply_markup,
                                                     PromisedQueryPtr query) mutable {
        if (!business_connection_id.empty()) {
          return check_business_connection_chat_id(
              business_connection_id, chat_id_str, std::move(query),
              [this, message_id, reply_markup = std::move(reply_markup), input_checklist = std::move(input_checklist)](
                  const BusinessConnection *business_connection, int64 chat_id, PromisedQueryPtr query) mutable {
                send_request(make_object<td_api::editBusinessMessageChecklist>(business_connection->id_, chat_id,
                                                                               message_id, std::move(reply_markup),
                                                                               std::move(input_checklist)),
                             td::make_unique<TdOnReturnBusinessMessageCallback>(this, business_connection->id_,
                                                                                std::move(query)));
              });
        }

        check_message(chat_id_str, message_id, false, AccessRights::Edit, "message to edit", std::move(query),
                      [this, reply_markup = std::move(reply_markup), input_checklist = std::move(input_checklist)](
                          int64 chat_id, int64 message_id, PromisedQueryPtr query) mutable {
                        send_request(make_object<td_api::editMessageChecklist>(
                                         chat_id, message_id, std::move(reply_markup), std::move(input_checklist)),
                                     td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
                      });
      });
  return td::Status::OK();
}

td::Status Client::process_edit_message_reply_markup_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id");
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(reply_markup, get_reply_markup(query.get(), bot_user_ids_));

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, inline_message_id = inline_message_id.str()](object_ptr<td_api::ReplyMarkup> reply_markup,
                                                            PromisedQueryPtr query) {
          send_request(make_object<td_api::editInlineMessageReplyMarkup>(inline_message_id, std::move(reply_markup)),
                       td::make_unique<TdOnEditInlineMessageCallback>(std::move(query)));
        });
  } else {
    resolve_reply_markup_bot_usernames(
        std::move(reply_markup), std::move(query),
        [this, business_connection_id = business_connection_id.str(), chat_id_str = chat_id.str(), message_id](
            object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) {
          if (!business_connection_id.empty()) {
            return check_business_connection_chat_id(
                business_connection_id, chat_id_str, std::move(query),
                [this, message_id, reply_markup = std::move(reply_markup)](
                    const BusinessConnection *business_connection, int64 chat_id, PromisedQueryPtr query) mutable {
                  send_request(make_object<td_api::editBusinessMessageReplyMarkup>(business_connection->id_, chat_id,
                                                                                   message_id, std::move(reply_markup)),
                               td::make_unique<TdOnReturnBusinessMessageCallback>(this, business_connection->id_,
                                                                                  std::move(query)));
                });
          }

          check_message(chat_id_str, message_id, false, AccessRights::Edit, "message to edit", std::move(query),
                        [this, reply_markup = std::move(reply_markup)](int64 chat_id, int64 message_id,
                                                                       PromisedQueryPtr query) mutable {
                          send_request(
                              make_object<td_api::editMessageReplyMarkup>(chat_id, message_id, std::move(reply_markup)),
                              td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
                        });
        });
  }
  return td::Status::OK();
}

td::Status Client::process_edit_ephemeral_message_text_query(PromisedQueryPtr &query) {
  object_ptr<td_api::InputMessageContent> input_message_text;
  if (query->has_arg("rich_message")) {
    TRY_RESULT(input_rich_message, get_input_rich_message(query.get()));
    input_message_text = make_object<td_api::inputMessageRichMessage>(std::move(input_rich_message), false);
  } else {
    TRY_RESULT_ASSIGN(input_message_text, get_input_message_text(query.get()));
  }
  return do_edit_ephemeral_message(std::move(input_message_text), query);
}

td::Status Client::process_edit_ephemeral_message_media_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_media, get_input_media(query.get(), "media"));
  return do_edit_ephemeral_message(std::move(input_media), query);
}

td::Status Client::process_edit_ephemeral_message_caption_query(PromisedQueryPtr &query) {
  TRY_RESULT(caption, get_caption(query.get()));
  auto show_caption_above_media = to_bool(query->arg("show_caption_above_media"));
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(receiver_user_id, get_user_id(query.get(), "receiver_user_id"));
  auto ephemeral_message_id = get_integer_arg(query.get(), "ephemeral_message_id", 0);
  TRY_RESULT(reply_markup, get_reply_markup(query.get(), bot_user_ids_));

  resolve_reply_markup_bot_usernames(
      std::move(reply_markup), std::move(query),
      [this, chat_id_str = chat_id.str(), receiver_user_id, ephemeral_message_id, caption = std::move(caption),
       show_caption_above_media](object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
        check_chat(
            chat_id_str, AccessRights::Edit, std::move(query),
            [this, receiver_user_id, ephemeral_message_id, caption = std::move(caption), show_caption_above_media,
             reply_markup = std::move(reply_markup)](int64 chat_id, PromisedQueryPtr query) mutable {
              send_request(make_object<td_api::editEphemeralMessageCaption>(
                               chat_id, receiver_user_id, ephemeral_message_id, std::move(reply_markup),
                               std::move(caption), show_caption_above_media),
                           td::make_unique<TdOnOkQueryCallback>(std::move(query)));
            });
      });
  return td::Status::OK();
}

td::Status Client::process_edit_ephemeral_message_reply_markup_query(PromisedQueryPtr &query) {
  return do_edit_ephemeral_message(nullptr, query);
}

td::Status Client::process_delete_message_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  check_message(chat_id, message_id, false, AccessRights::Write, "message to delete", std::move(query),
                [this](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                  send_request(make_object<td_api::deleteMessages>(chat_id, td::vector<int64>{message_id}, true),
                               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                });
  return td::Status::OK();
}

td::Status Client::process_delete_messages_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(message_ids, get_message_ids(query.get(), 100));
  check_messages(chat_id, std::move(message_ids), true, AccessRights::Write, "message to delete", std::move(query),
                 [this](int64 chat_id, td::vector<int64> message_ids, PromisedQueryPtr query) {
                   send_request(make_object<td_api::deleteMessages>(chat_id, std::move(message_ids), true),
                                td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                 });
  return td::Status::OK();
}

td::Status Client::process_delete_ephemeral_message_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(receiver_user_id, get_user_id(query.get(), "receiver_user_id"));
  auto ephemeral_message_id = get_integer_arg(query.get(), "ephemeral_message_id", 0);
  check_chat(chat_id, AccessRights::Read, std::move(query),
             [this, receiver_user_id, ephemeral_message_id](int64 chat_id, PromisedQueryPtr query) {
               send_request(
                   make_object<td_api::deleteEphemeralMessage>(chat_id, receiver_user_id, ephemeral_message_id),
                   td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_delete_message_reaction_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  int64 user_id = -1;
  td::string actor_chat_id;
  if (!query->arg("user_id").empty()) {
    TRY_RESULT_ASSIGN(user_id, get_user_id(query.get()));
  } else {
    actor_chat_id = query->arg("actor_chat_id").str();
  }
  check_message(chat_id, message_id, false, AccessRights::Write, "message to delete reactions", std::move(query),
                [this, user_id, actor_chat_id](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                  if (user_id >= 0) {
                    check_user_no_fail(
                        user_id, std::move(query), [this, chat_id, message_id, user_id](PromisedQueryPtr query) {
                          send_request(make_object<td_api::deleteMessageReactionsFromSender>(
                                           chat_id, message_id, make_object<td_api::messageSenderUser>(user_id)),
                                       td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                        });
                  } else {
                    check_chat_no_fail(
                        actor_chat_id, std::move(query),
                        [this, chat_id, message_id](int64 actor_chat_id, PromisedQueryPtr query) {
                          send_request(make_object<td_api::deleteMessageReactionsFromSender>(
                                           chat_id, message_id, make_object<td_api::messageSenderChat>(actor_chat_id)),
                                       td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                        });
                  }
                });
  return td::Status::OK();
}

td::Status Client::process_delete_all_message_reactions_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  int64 user_id = -1;
  td::string actor_chat_id;
  if (!query->arg("user_id").empty()) {
    TRY_RESULT_ASSIGN(user_id, get_user_id(query.get()));
  } else {
    actor_chat_id = query->arg("actor_chat_id").str();
  }
  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, user_id, actor_chat_id](int64 chat_id, PromisedQueryPtr query) {
               if (user_id >= 0) {
                 check_user_no_fail(user_id, std::move(query), [this, chat_id, user_id](PromisedQueryPtr query) {
                   send_request(make_object<td_api::deleteAllRecentMessageReactionsFromSender>(
                                    chat_id, make_object<td_api::messageSenderUser>(user_id)),
                                td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                 });
               } else {
                 check_chat_no_fail(actor_chat_id, std::move(query),
                                    [this, chat_id](int64 actor_chat_id, PromisedQueryPtr query) {
                                      send_request(make_object<td_api::deleteAllRecentMessageReactionsFromSender>(
                                                       chat_id, make_object<td_api::messageSenderChat>(actor_chat_id)),
                                                   td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                                    });
               }
             });
  return td::Status::OK();
}

td::Status Client::process_post_story_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  TRY_RESULT(content, get_input_story_content(query.get()));
  TRY_RESULT(areas, get_input_story_areas(query.get()));
  TRY_RESULT(caption, get_caption(query.get()));
  check_business_connection(
      business_connection_id, std::move(query),
      [this, content = std::move(content), areas = std::move(areas), caption = std::move(caption)](
          const BusinessConnection *business_connection, PromisedQueryPtr query) mutable {
        auto active_period = get_integer_arg(query.get(), "active_period", 0, 0, 1000000000);
        auto is_posted_to_chat_page = to_bool(query->arg("post_to_chat_page"));
        auto protect_content = to_bool(query->arg("protect_content"));
        send_request(make_object<td_api::postStory>(
                         business_connection->user_chat_id_, std::move(content), std::move(areas), std::move(caption),
                         make_object<td_api::storyPrivacySettingsEveryone>(), td::vector<int32>(), active_period,
                         nullptr, is_posted_to_chat_page, protect_content),
                     td::make_unique<TdOnPostStoryCallback>(this, std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_repost_story_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  TRY_RESULT(from_chat_id, get_required_string_arg(query.get(), "from_chat_id"));
  check_business_connection(
      business_connection_id, std::move(query),
      [this, from_chat_id = std::move(from_chat_id)](const BusinessConnection *business_connection,
                                                     PromisedQueryPtr query) {
        auto chat_id = business_connection->user_chat_id_;
        check_chat(from_chat_id, AccessRights::Read, std::move(query),
                   [this, chat_id](int64 from_chat_id, PromisedQueryPtr query) {
                     auto from_story_id = get_integer_arg(query.get(), "story_id", 0, 0, 1000000000);
                     auto active_period = get_integer_arg(query.get(), "active_period", 0, 0, 1000000000);
                     auto is_posted_to_chat_page = to_bool(query->arg("post_to_chat_page"));
                     auto protect_content = to_bool(query->arg("protect_content"));
                     send_request(make_object<td_api::postStory>(
                                      chat_id, nullptr, nullptr, nullptr,
                                      make_object<td_api::storyPrivacySettingsEveryone>(), td::vector<int32>(),
                                      active_period, make_object<td_api::storyFullId>(from_chat_id, from_story_id),
                                      is_posted_to_chat_page, protect_content),
                                  td::make_unique<TdOnRepostStoryCallback>(this, std::move(query)));
                   });
      });
  return td::Status::OK();
}

td::Status Client::process_edit_story_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  TRY_RESULT(content, get_input_story_content(query.get()));
  TRY_RESULT(areas, get_input_story_areas(query.get()));
  TRY_RESULT(caption, get_caption(query.get()));
  check_business_connection(
      business_connection_id, std::move(query),
      [this, content = std::move(content), areas = std::move(areas), caption = std::move(caption)](
          const BusinessConnection *business_connection, PromisedQueryPtr query) mutable {
        auto story_id = get_integer_arg(query.get(), "story_id", 0, 0, 1000000000);
        send_request(make_object<td_api::editBusinessStory>(business_connection->user_chat_id_, story_id,
                                                            std::move(content), std::move(areas), std::move(caption),
                                                            make_object<td_api::storyPrivacySettingsEveryone>()),
                     td::make_unique<TdOnGetStoryCallback>(this, std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_delete_story_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  check_business_connection(business_connection_id, std::move(query),
                            [this](const BusinessConnection *business_connection, PromisedQueryPtr query) {
                              auto story_id = get_integer_arg(query.get(), "story_id", 0, 0, 1000000000);
                              send_request(make_object<td_api::deleteBusinessStory>(business_connection->id_, story_id),
                                           td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                            });
  return td::Status::OK();
}

td::Status Client::process_create_invoice_link_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_message_invoice, get_input_message_invoice(query.get()));
  auto business_connection_id = query->arg("business_connection_id").str();
  if (!business_connection_id.empty()) {
    check_business_connection(business_connection_id, std::move(query),
                              [this, input_message_invoice = std::move(input_message_invoice)](
                                  const BusinessConnection *business_connection, PromisedQueryPtr query) mutable {
                                send_request(make_object<td_api::createInvoiceLink>(business_connection->id_,
                                                                                    std::move(input_message_invoice)),
                                             td::make_unique<TdOnCreateInvoiceLinkCallback>(std::move(query)));
                              });
    return td::Status::OK();
  }
  send_request(make_object<td_api::createInvoiceLink>(td::string(), std::move(input_message_invoice)),
               td::make_unique<TdOnCreateInvoiceLinkCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_get_my_star_balance_query(PromisedQueryPtr &query) {
  send_request(make_object<td_api::getStarTransactions>(make_object<td_api::messageSenderUser>(my_id_), td::string(),
                                                        nullptr, td::string(), 0),
               td::make_unique<TdOnGetMyStarBalanceCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_get_star_transactions_query(PromisedQueryPtr &query) {
  auto offset = get_integer_arg(query.get(), "offset", 0, 0);
  auto limit = get_integer_arg(query.get(), "limit", 100, 1, 100);
  send_request(make_object<td_api::getStarTransactions>(make_object<td_api::messageSenderUser>(my_id_), td::string(),
                                                        nullptr, td::to_string(offset), limit),
               td::make_unique<TdOnGetStarTransactionsCallback>(this, std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_refund_star_payment_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  TRY_RESULT(telegram_payment_charge_id, get_required_string_arg(query.get(), "telegram_payment_charge_id"));
  check_user_no_fail(
      user_id, std::move(query),
      [this, telegram_payment_charge_id = telegram_payment_charge_id.str(), user_id](PromisedQueryPtr query) {
        send_request(make_object<td_api::refundStarPayment>(user_id, telegram_payment_charge_id),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_edit_user_star_subscription_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto telegram_payment_charge_id = query->arg("telegram_payment_charge_id");
  auto is_canceled = to_bool(query->arg("is_canceled"));
  check_user_no_fail(
      user_id, std::move(query),
      [this, user_id, telegram_payment_charge_id = telegram_payment_charge_id.str(),
       is_canceled](PromisedQueryPtr query) {
        send_request(make_object<td_api::editUserStarSubscription>(user_id, telegram_payment_charge_id, is_canceled),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_get_available_gifts_query(PromisedQueryPtr &query) {
  send_request(make_object<td_api::getAvailableGifts>(), td::make_unique<TdOnGetGiftsCallback>(this, std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_send_gift_query(PromisedQueryPtr &query) {
  auto gift_id = td::to_integer<int64>(query->arg("gift_id"));
  auto pay_for_upgrade = to_bool(query->arg("pay_for_upgrade"));
  TRY_RESULT(text, get_formatted_text(query->arg("text").str(), query->arg("text_parse_mode").str(),
                                      get_input_entities(query.get(), "text_entities")));
  if (query->has_arg("chat_id") && !query->arg("chat_id").empty()) {
    auto chat_id = query->arg("chat_id");
    check_chat(chat_id, AccessRights::Read, std::move(query),
               [this, gift_id, pay_for_upgrade, text = std::move(text)](int64 chat_id, PromisedQueryPtr query) mutable {
                 send_request(make_object<td_api::sendGift>(gift_id, make_object<td_api::messageSenderChat>(chat_id),
                                                            std::move(text), false, pay_for_upgrade),
                              td::make_unique<TdOnOkQueryCallback>(std::move(query)));
               });
  } else {
    TRY_RESULT(user_id, get_user_id(query.get()));
    check_user(user_id, std::move(query),
               [this, gift_id, pay_for_upgrade, user_id, text = std::move(text)](PromisedQueryPtr query) mutable {
                 send_request(make_object<td_api::sendGift>(gift_id, make_object<td_api::messageSenderUser>(user_id),
                                                            std::move(text), false, pay_for_upgrade),
                              td::make_unique<TdOnOkQueryCallback>(std::move(query)));
               });
  }
  return td::Status::OK();
}

td::Status Client::process_gift_premium_subscription_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto star_count = td::to_integer<int64>(query->arg("star_count"));
  auto month_count = get_integer_arg(query.get(), "month_count", 0, 0, 1000);
  TRY_RESULT(text, get_formatted_text(query->arg("text").str(), query->arg("text_parse_mode").str(),
                                      get_input_entities(query.get(), "text_entities")));
  check_user(user_id, std::move(query),
             [this, star_count, month_count, user_id, text = std::move(text)](PromisedQueryPtr query) mutable {
               send_request(
                   make_object<td_api::giftPremiumWithStars>(user_id, star_count, month_count, std::move(text)),
                   td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_get_managed_bot_token_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  check_user(user_id, std::move(query), [this, user_id](PromisedQueryPtr query) {
    send_request(make_object<td_api::getManagedBotToken>(user_id, false),
                 td::make_unique<TdOnGetBotTokenCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_replace_managed_bot_token_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  check_user(user_id, std::move(query), [this, user_id](PromisedQueryPtr query) {
    send_request(make_object<td_api::getManagedBotToken>(user_id, true),
                 td::make_unique<TdOnGetBotTokenCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_get_managed_bot_access_settings_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  check_user(user_id, std::move(query), [this, user_id](PromisedQueryPtr query) {
    send_request(make_object<td_api::getManagedBotAccessSettings>(user_id),
                 td::make_unique<TdOnGetBotAccessSettingsCallback>(this, std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_set_managed_bot_access_settings_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto is_restricted = to_bool(query->arg("is_access_restricted"));
  td::vector<int64> added_user_ids;
  if (is_restricted && query->has_arg("added_user_ids")) {
    TRY_RESULT_ASSIGN(added_user_ids, get_user_ids(query.get(), 10u, "added_user_ids"));
  }
  check_user(user_id, std::move(query),
             [this, user_id,
              access_settings = make_object<td_api::botAccessSettings>(is_restricted, std::move(added_user_ids))](
                 PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::setManagedBotAccessSettings>(user_id, std::move(access_settings)),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_verify_user_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto custom_description = query->arg("custom_description");
  check_user(user_id, std::move(query),
             [this, user_id, custom_description = custom_description.str()](PromisedQueryPtr query) {
               send_request(make_object<td_api::setMessageSenderBotVerification>(
                                0, make_object<td_api::messageSenderUser>(user_id), custom_description),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_verify_chat_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto custom_description = query->arg("custom_description");
  check_chat(chat_id, AccessRights::Read, std::move(query),
             [this, custom_description = custom_description.str()](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::setMessageSenderBotVerification>(
                                0, make_object<td_api::messageSenderChat>(chat_id), custom_description),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_remove_user_verification_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  check_user_no_fail(user_id, std::move(query), [this, user_id](PromisedQueryPtr query) {
    send_request(
        make_object<td_api::removeMessageSenderBotVerification>(0, make_object<td_api::messageSenderUser>(user_id)),
        td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_remove_chat_verification_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  check_chat_no_fail(chat_id, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(
        make_object<td_api::removeMessageSenderBotVerification>(0, make_object<td_api::messageSenderChat>(chat_id)),
        td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_set_game_score_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto score = td::to_integer<int32>(query->arg("score"));
  auto force = to_bool(query->arg("force"));
  bool edit_message = true;
  if (query->has_arg("disable_edit_message")) {
    edit_message = !to_bool(query->arg("disable_edit_message"));
  } else if (query->has_arg("edit_message")) {
    edit_message = to_bool(query->arg("edit_message"));
  }

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    check_user_no_fail(
        user_id, std::move(query),
        [this, inline_message_id = inline_message_id.str(), edit_message, user_id, score,
         force](PromisedQueryPtr query) {
          send_request(make_object<td_api::setInlineGameScore>(inline_message_id, edit_message, user_id, score, force),
                       td::make_unique<TdOnEditInlineMessageCallback>(std::move(query)));
        });
  } else {
    check_message(chat_id, message_id, false, AccessRights::Edit, "message to set game score", std::move(query),
                  [this, user_id, score, force, edit_message](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                    check_user_no_fail(
                        user_id, std::move(query),
                        [this, chat_id, message_id, user_id, score, force, edit_message](PromisedQueryPtr query) {
                          send_request(make_object<td_api::setGameScore>(chat_id, message_id, edit_message, user_id,
                                                                         score, force),
                                       td::make_unique<TdOnEditMessageCallback>(this, std::move(query)));
                        });
                  });
  }
  return td::Status::OK();
}

td::Status Client::process_get_game_high_scores_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  TRY_RESULT(user_id, get_user_id(query.get()));

  if (chat_id.empty() && message_id == 0) {
    TRY_RESULT(inline_message_id, get_inline_message_id(query.get()));
    check_user_no_fail(user_id, std::move(query),
                       [this, inline_message_id = inline_message_id.str(), user_id](PromisedQueryPtr query) {
                         send_request(make_object<td_api::getInlineGameHighScores>(inline_message_id, user_id),
                                      td::make_unique<TdOnGetGameHighScoresCallback>(this, std::move(query)));
                       });
  } else {
    check_message(chat_id, message_id, false, AccessRights::Read, "message to get game high scores", std::move(query),
                  [this, user_id](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                    check_user_no_fail(
                        user_id, std::move(query), [this, chat_id, message_id, user_id](PromisedQueryPtr query) {
                          send_request(make_object<td_api::getGameHighScores>(chat_id, message_id, user_id),
                                       td::make_unique<TdOnGetGameHighScoresCallback>(this, std::move(query)));
                        });
                  });
  }
  return td::Status::OK();
}

td::Status Client::process_answer_web_app_query_query(PromisedQueryPtr &query) {
  auto web_app_query_id = query->arg("web_app_query_id");

  TRY_RESULT(result, get_inline_query_result(query.get(), bot_user_ids_));
  td::vector<object_ptr<td_api::InputInlineQueryResult>> results;
  results.push_back(std::move(result));

  resolve_inline_query_results_bot_usernames(
      std::move(results), std::move(query),
      [this, web_app_query_id = web_app_query_id.str()](td::vector<object_ptr<td_api::InputInlineQueryResult>> results,
                                                        PromisedQueryPtr query) {
        CHECK(results.size() == 1);
        send_request(make_object<td_api::answerWebAppQuery>(web_app_query_id, std::move(results[0])),
                     td::make_unique<TdOnAnswerWebAppQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_answer_guest_query_query(PromisedQueryPtr &query) {
  auto guest_query_id = td::to_integer<int64>(query->arg("guest_query_id"));
  TRY_RESULT(result, get_inline_query_result(query.get(), bot_user_ids_));
  td::vector<object_ptr<td_api::InputInlineQueryResult>> results;
  results.push_back(std::move(result));

  resolve_inline_query_results_bot_usernames(
      std::move(results), std::move(query),
      [this, guest_query_id](td::vector<object_ptr<td_api::InputInlineQueryResult>> results, PromisedQueryPtr query) {
        CHECK(results.size() == 1);
        send_request(make_object<td_api::answerGuestQuery>(guest_query_id, std::move(results[0])),
                     td::make_unique<TdOnAnswerWebAppQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_answer_inline_query_query(PromisedQueryPtr &query) {
  auto inline_query_id = td::to_integer<int64>(query->arg("inline_query_id"));
  auto is_personal = to_bool(query->arg("is_personal"));
  int32 cache_time = get_integer_arg(query.get(), "cache_time", 300, 0, 24 * 60 * 60);
  auto next_offset = query->arg("next_offset");
  TRY_RESULT(button, get_inline_query_results_button(query->arg("button")));
  if (button == nullptr) {
    auto switch_pm_text = query->arg("switch_pm_text");
    if (!switch_pm_text.empty()) {
      button = make_object<td_api::inlineQueryResultsButton>(
          switch_pm_text.str(),
          make_object<td_api::inlineQueryResultsButtonTypeStartBot>(query->arg("switch_pm_parameter").str()));
    }
  }
  TRY_RESULT(results, get_inline_query_results(query.get(), bot_user_ids_));

  resolve_inline_query_results_bot_usernames(
      std::move(results), std::move(query),
      [this, inline_query_id, is_personal, cache_time, next_offset = next_offset.str(), button = std::move(button)](
          td::vector<object_ptr<td_api::InputInlineQueryResult>> results, PromisedQueryPtr query) mutable {
        send_request(make_object<td_api::answerInlineQuery>(inline_query_id, is_personal, std::move(button),
                                                            std::move(results), cache_time, next_offset),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_answer_chat_join_request_query_query(PromisedQueryPtr &query) {
  auto query_id = td::to_integer<int64>(query->arg("chat_join_request_query_id"));
  auto result_str = td::to_lower(td::trim(query->arg("result")));
  object_ptr<td_api::ChatJoinRequestResult> result;
  if (result_str == "approve") {
    result = make_object<td_api::chatJoinRequestResultApproved>();
  } else if (result_str == "decline") {
    result = make_object<td_api::chatJoinRequestResultDeclined>();
  } else if (result_str == "queue") {
    result = make_object<td_api::chatJoinRequestResultQueued>();
  } else {
    return td::Status::Error(400, "Invalid query result specified");
  }

  send_request(make_object<td_api::answerChatJoinRequestQuery>(query_id, std::move(result), td::string()),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_send_chat_join_request_web_app_query(PromisedQueryPtr &query) {
  auto query_id = td::to_integer<int64>(query->arg("chat_join_request_query_id"));
  TRY_RESULT(web_app_url, get_required_string_arg(query.get(), "web_app_url"));
  send_request(make_object<td_api::answerChatJoinRequestQuery>(query_id, nullptr, web_app_url.str()),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_save_prepared_inline_message_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  TRY_RESULT(result, get_inline_query_result(query.get(), bot_user_ids_));
  auto allow_user_chats = to_bool(query->arg("allow_user_chats"));
  auto allow_bot_chats = to_bool(query->arg("allow_bot_chats"));
  auto allow_group_chats = to_bool(query->arg("allow_group_chats"));
  auto allow_channel_chats = to_bool(query->arg("allow_channel_chats"));
  auto types =
      make_object<td_api::targetChatTypes>(allow_user_chats, allow_bot_chats, allow_group_chats, allow_channel_chats);

  td::vector<object_ptr<td_api::InputInlineQueryResult>> results;
  results.push_back(std::move(result));

  resolve_inline_query_results_bot_usernames(
      std::move(results), std::move(query),
      [this, user_id, types = std::move(types)](td::vector<object_ptr<td_api::InputInlineQueryResult>> results,
                                                PromisedQueryPtr query) mutable {
        CHECK(results.size() == 1);
        send_request(make_object<td_api::savePreparedInlineMessage>(user_id, std::move(results[0]), std::move(types)),
                     td::make_unique<TdOnSavePreparedInlineMessageCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_save_prepared_keyboard_button_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto r_value = json_decode(query->arg("button"));
  if (r_value.is_error()) {
    return td::Status::Error(400, "Can't parse keyboard button JSON object");
  }
  auto value = r_value.move_as_ok();
  auto r_button = get_keyboard_button(std::move(value));
  if (r_button.is_error()) {
    return td::Status::Error(400, PSLICE() << "Can't parse keyboard button: " << r_button.error().message());
  }

  send_request(make_object<td_api::savePreparedKeyboardButton>(user_id, r_button.move_as_ok()),
               td::make_unique<TdOnSavePreparedKeyboardButtonCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_answer_callback_query_query(PromisedQueryPtr &query) {
  auto callback_query_id = td::to_integer<int64>(query->arg("callback_query_id"));
  td::string text = query->arg("text").str();
  bool show_alert = to_bool(query->arg("show_alert"));
  td::string url = query->arg("url").str();
  int32 cache_time = get_integer_arg(query.get(), "cache_time", 0, 0, 24 * 30 * 60 * 60);

  send_request(make_object<td_api::answerCallbackQuery>(callback_query_id, text, show_alert, url, cache_time),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_answer_shipping_query_query(PromisedQueryPtr &query) {
  auto shipping_query_id = td::to_integer<int64>(query->arg("shipping_query_id"));
  auto ok = to_bool(query->arg("ok"));
  td::vector<object_ptr<td_api::shippingOption>> shipping_options;
  td::MutableSlice error_message;
  if (ok) {
    TRY_RESULT_ASSIGN(shipping_options, get_shipping_options(query.get()));
  } else {
    TRY_RESULT_ASSIGN(error_message, get_required_string_arg(query.get(), "error_message"));
  }
  send_request(
      make_object<td_api::answerShippingQuery>(shipping_query_id, std::move(shipping_options), error_message.str()),
      td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_answer_pre_checkout_query_query(PromisedQueryPtr &query) {
  auto pre_checkout_query_id = td::to_integer<int64>(query->arg("pre_checkout_query_id"));
  auto ok = to_bool(query->arg("ok"));
  td::MutableSlice error_message;
  if (!ok) {
    TRY_RESULT_ASSIGN(error_message, get_required_string_arg(query.get(), "error_message"));
  }

  send_request(make_object<td_api::answerPreCheckoutQuery>(pre_checkout_query_id, error_message.str()),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_export_chat_invite_link_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::replacePrimaryChatInviteLink>(chat_id),
                 td::make_unique<TdOnReplacePrimaryChatInviteLinkCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_create_chat_invite_link_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto name = query->arg("name");
  auto expire_date = get_integer_arg(query.get(), "expire_date", 0, 0);
  auto member_limit = get_integer_arg(query.get(), "member_limit", 0, 0, 100000);
  auto creates_join_request = to_bool(query->arg("creates_join_request"));

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, name = name.str(), expire_date, member_limit, creates_join_request](int64 chat_id,
                                                                                        PromisedQueryPtr query) {
               send_request(make_object<td_api::createChatInviteLink>(chat_id, name, expire_date, member_limit,
                                                                      creates_join_request),
                            td::make_unique<TdOnGetChatInviteLinkCallback>(this, std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_create_chat_subscription_invite_link_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto name = query->arg("name");
  auto subscription_period = get_integer_arg(query.get(), "subscription_period", 0, 0, 1000000000);
  auto subscription_price = get_integer_arg(query.get(), "subscription_price", 0, 0, 1000000);

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, name = name.str(), subscription_period, subscription_price](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::createChatSubscriptionInviteLink>(
                                chat_id, name,
                                make_object<td_api::starSubscriptionPricing>(subscription_period, subscription_price)),
                            td::make_unique<TdOnGetChatInviteLinkCallback>(this, std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_edit_chat_invite_link_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto invite_link = query->arg("invite_link");
  auto name = query->arg("name");
  auto expire_date = get_integer_arg(query.get(), "expire_date", 0, 0);
  auto member_limit = get_integer_arg(query.get(), "member_limit", 0, 0, 100000);
  auto creates_join_request = to_bool(query->arg("creates_join_request"));

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, invite_link = invite_link.str(), name = name.str(), expire_date, member_limit,
              creates_join_request](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::editChatInviteLink>(chat_id, invite_link, name, expire_date,
                                                                    member_limit, creates_join_request),
                            td::make_unique<TdOnGetChatInviteLinkCallback>(this, std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_edit_chat_subscription_invite_link_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto invite_link = query->arg("invite_link");
  auto name = query->arg("name");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, invite_link = invite_link.str(), name = name.str()](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::editChatSubscriptionInviteLink>(chat_id, invite_link, name),
                            td::make_unique<TdOnGetChatInviteLinkCallback>(this, std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_revoke_chat_invite_link_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto invite_link = query->arg("invite_link");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, invite_link = invite_link.str()](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::revokeChatInviteLink>(chat_id, invite_link),
                            td::make_unique<TdOnGetChatInviteLinkCallback>(this, std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_get_business_connection_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id");
  check_business_connection(business_connection_id.str(), std::move(query),
                            [this](const BusinessConnection *business_connection, PromisedQueryPtr query) {
                              answer_query(JsonBusinessConnection(business_connection, this), std::move(query));
                            });
  return td::Status::OK();
}

td::Status Client::process_read_business_message_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  auto chat_id_str = query->arg("chat_id").str();
  auto message_id = get_message_id(query.get());
  check_business_connection_chat_id(
      business_connection_id, chat_id_str, std::move(query),
      [this, message_id](const BusinessConnection *business_connection, int64 chat_id, PromisedQueryPtr query) {
        send_request(make_object<td_api::readBusinessMessage>(business_connection->id_, chat_id, message_id),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_delete_business_messages_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  TRY_RESULT(message_ids, get_message_ids(query.get(), 100));
  check_business_connection(
      business_connection_id, std::move(query),
      [this, message_ids = std::move(message_ids)](const BusinessConnection *business_connection,
                                                   PromisedQueryPtr query) mutable {
        send_request(make_object<td_api::deleteBusinessMessages>(business_connection->id_, std::move(message_ids)),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_set_business_account_name_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  TRY_RESULT(first_name, get_required_string_arg(query.get(), "first_name"));
  auto last_name = query->arg("last_name");
  check_business_connection(
      business_connection_id, std::move(query),
      [this, first_name, last_name](const BusinessConnection *business_connection, PromisedQueryPtr query) {
        send_request(
            make_object<td_api::setBusinessAccountName>(business_connection->id_, first_name.str(), last_name.str()),
            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_set_business_account_username_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  auto username = query->arg("username");
  check_business_connection(
      business_connection_id, std::move(query),
      [this, username](const BusinessConnection *business_connection, PromisedQueryPtr query) {
        send_request(make_object<td_api::setBusinessAccountUsername>(business_connection->id_, username.str()),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_set_business_account_bio_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  auto bio = query->arg("bio");
  check_business_connection(business_connection_id, std::move(query),
                            [this, bio](const BusinessConnection *business_connection, PromisedQueryPtr query) {
                              send_request(
                                  make_object<td_api::setBusinessAccountBio>(business_connection->id_, bio.str()),
                                  td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                            });
  return td::Status::OK();
}

td::Status Client::process_set_business_account_profile_photo_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  TRY_RESULT(photo, get_input_chat_photo(query.get()));
  auto is_public = to_bool(query->arg("is_public"));
  check_business_connection(business_connection_id, std::move(query),
                            [this, photo = std::move(photo), is_public](const BusinessConnection *business_connection,
                                                                        PromisedQueryPtr query) mutable {
                              send_request(make_object<td_api::setBusinessAccountProfilePhoto>(
                                               business_connection->id_, std::move(photo), is_public),
                                           td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                            });
  return td::Status::OK();
}

td::Status Client::process_remove_business_account_profile_photo_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  auto is_public = to_bool(query->arg("is_public"));
  check_business_connection(
      business_connection_id, std::move(query),
      [this, is_public](const BusinessConnection *business_connection, PromisedQueryPtr query) {
        send_request(make_object<td_api::setBusinessAccountProfilePhoto>(business_connection->id_, nullptr, is_public),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_set_business_account_gift_settings_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  auto show_gift_button = to_bool(query->arg("show_gift_button"));
  TRY_RESULT(accepted_gift_types, get_accepted_gift_types(query.get()));
  auto settings = make_object<td_api::giftSettings>(show_gift_button, std::move(accepted_gift_types));
  check_business_connection(
      business_connection_id, std::move(query),
      [this, settings = std::move(settings)](const BusinessConnection *business_connection,
                                             PromisedQueryPtr query) mutable {
        send_request(make_object<td_api::setBusinessAccountGiftSettings>(business_connection->id_, std::move(settings)),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_get_business_account_star_balance_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  check_business_connection(business_connection_id, std::move(query),
                            [this](const BusinessConnection *business_connection, PromisedQueryPtr query) {
                              send_request(make_object<td_api::getBusinessAccountStarAmount>(business_connection->id_),
                                           td::make_unique<TdOnGetStarAmountCallback>(std::move(query)));
                            });
  return td::Status::OK();
}

td::Status Client::process_transfer_business_account_stars_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  auto star_count = get_integer_arg(query.get(), "star_count", 0, 0, 1000000000);
  check_business_connection(
      business_connection_id, std::move(query),
      [this, star_count](const BusinessConnection *business_connection, PromisedQueryPtr query) {
        send_request(make_object<td_api::transferBusinessAccountStars>(business_connection->id_, star_count),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_get_business_account_gifts_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  check_business_connection(
      business_connection_id, std::move(query),
      [this](const BusinessConnection *business_connection, PromisedQueryPtr query) {
        auto exclude_unsaved = to_bool(query->arg("exclude_unsaved"));
        auto exclude_saved = to_bool(query->arg("exclude_saved"));
        auto exclude_unlimited = to_bool(query->arg("exclude_unlimited"));
        auto exclude_limited = to_bool(query->arg("exclude_limited"));
        auto exclude_limited_upgradable = to_bool(query->arg("exclude_limited_upgradable"));
        auto exclude_limited_non_upgradable = to_bool(query->arg("exclude_limited_non_upgradable"));
        auto exclude_upgraded = to_bool(query->arg("exclude_unique"));
        auto exclude_hosted = to_bool(query->arg("exclude_from_blockchain"));
        auto sort_by_price = to_bool(query->arg("sort_by_price"));
        auto offset = query->arg("offset");
        auto limit = get_integer_arg(query.get(), "limit", 100, 1, 100);
        if (exclude_limited) {
          exclude_limited_upgradable = true;
          exclude_limited_non_upgradable = true;
        }
        send_request(make_object<td_api::getReceivedGifts>(
                         business_connection->id_, make_object<td_api::messageSenderUser>(my_id_), 0, exclude_unsaved,
                         exclude_saved, exclude_unlimited, exclude_limited_upgradable, exclude_limited_non_upgradable,
                         exclude_upgraded, false, exclude_hosted, sort_by_price, offset.str(), limit),
                     td::make_unique<TdOnGetReceivedGiftsCallback>(this, true, std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_get_user_gifts_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  check_user(user_id, std::move(query), [this, user_id](PromisedQueryPtr query) {
    auto exclude_unlimited = to_bool(query->arg("exclude_unlimited"));
    auto exclude_limited_upgradable = to_bool(query->arg("exclude_limited_upgradable"));
    auto exclude_limited_non_upgradable = to_bool(query->arg("exclude_limited_non_upgradable"));
    auto exclude_upgraded = to_bool(query->arg("exclude_unique"));
    auto exclude_hosted = to_bool(query->arg("exclude_from_blockchain"));
    auto sort_by_price = to_bool(query->arg("sort_by_price"));
    auto offset = query->arg("offset");
    auto limit = get_integer_arg(query.get(), "limit", 100, 1, 100);
    send_request(make_object<td_api::getReceivedGifts>(td::string(), make_object<td_api::messageSenderUser>(user_id), 0,
                                                       true, false, exclude_unlimited, exclude_limited_upgradable,
                                                       exclude_limited_non_upgradable, exclude_upgraded, false,
                                                       exclude_hosted, sort_by_price, offset.str(), limit),
                 td::make_unique<TdOnGetReceivedGiftsCallback>(this, false, std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_get_chat_gifts_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  check_chat(chat_id, AccessRights::Read, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    auto exclude_unsaved = to_bool(query->arg("exclude_unsaved"));
    auto exclude_saved = to_bool(query->arg("exclude_saved"));
    auto exclude_unlimited = to_bool(query->arg("exclude_unlimited"));
    auto exclude_limited_upgradable = to_bool(query->arg("exclude_limited_upgradable"));
    auto exclude_limited_non_upgradable = to_bool(query->arg("exclude_limited_non_upgradable"));
    auto exclude_upgraded = to_bool(query->arg("exclude_unique"));
    auto exclude_hosted = to_bool(query->arg("exclude_from_blockchain"));
    auto sort_by_price = to_bool(query->arg("sort_by_price"));
    auto offset = query->arg("offset");
    auto limit = get_integer_arg(query.get(), "limit", 100, 1, 100);
    send_request(make_object<td_api::getReceivedGifts>(
                     td::string(), make_object<td_api::messageSenderChat>(chat_id), 0, exclude_unsaved, exclude_saved,
                     exclude_unlimited, exclude_limited_upgradable, exclude_limited_non_upgradable, exclude_upgraded,
                     false, exclude_hosted, sort_by_price, offset.str(), limit),
                 td::make_unique<TdOnGetReceivedGiftsCallback>(this, false, std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_convert_gift_to_stars_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  auto owned_gift_id = query->arg("owned_gift_id");
  check_business_connection(
      business_connection_id, std::move(query),
      [this, owned_gift_id](const BusinessConnection *business_connection, PromisedQueryPtr query) {
        send_request(make_object<td_api::sellGift>(business_connection->id_, owned_gift_id.str()),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_upgrade_gift_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  auto owned_gift_id = query->arg("owned_gift_id");
  auto keep_original_details = to_bool(query->arg("keep_original_details"));
  auto star_count = get_integer_arg(query.get(), "star_count", 0, 0, 1000000);
  check_business_connection(business_connection_id, std::move(query),
                            [this, owned_gift_id, keep_original_details, star_count](
                                const BusinessConnection *business_connection, PromisedQueryPtr query) {
                              send_request(
                                  make_object<td_api::upgradeGift>(business_connection->id_, owned_gift_id.str(),
                                                                   keep_original_details, star_count),
                                  td::make_unique<TdOnUpgradeGiftCallback>(std::move(query)));
                            });
  return td::Status::OK();
}

td::Status Client::process_transfer_gift_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id").str();
  auto owned_gift_id = query->arg("owned_gift_id");
  auto chat_id_str = query->arg("new_owner_chat_id").str();
  auto star_count = get_integer_arg(query.get(), "star_count", 0, 0, 1000000);
  check_business_connection_chat_id(
      business_connection_id, chat_id_str, std::move(query),
      [this, owned_gift_id, star_count](const BusinessConnection *business_connection, int64 chat_id,
                                        PromisedQueryPtr query) {
        send_request(make_object<td_api::transferGift>(business_connection->id_, owned_gift_id.str(),
                                                       make_object<td_api::messageSenderChat>(chat_id), star_count),
                     td::make_unique<TdOnOkQueryCallback>(std::move(query)));
      });
  return td::Status::OK();
}

td::Status Client::process_set_user_emoji_status_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto emoji_status_custom_emoji_id = td::to_integer<int64>(query->arg("emoji_status_custom_emoji_id"));
  auto emoji_status_expiration_date = td::to_integer<int32>(query->arg("emoji_status_expiration_date"));
  check_user(user_id, std::move(query),
             [this, user_id, emoji_status_custom_emoji_id, emoji_status_expiration_date](PromisedQueryPtr query) {
               send_request(
                   make_object<td_api::setUserEmojiStatus>(
                       user_id, emoji_status_custom_emoji_id == 0
                                    ? nullptr
                                    : make_object<td_api::emojiStatus>(
                                          make_object<td_api::emojiStatusTypeCustomEmoji>(emoji_status_custom_emoji_id),
                                          emoji_status_expiration_date)),
                   td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_get_chat_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Read, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    auto chat_info = get_chat(chat_id);
    CHECK(chat_info != nullptr);
    switch (chat_info->type) {
      case ChatInfo::Type::Private:
        return send_request(make_object<td_api::getUserFullInfo>(chat_info->user_id),
                            td::make_unique<TdOnGetChatFullInfoCallback>(this, chat_id, std::move(query)));
      case ChatInfo::Type::Group:
        return send_request(make_object<td_api::getBasicGroupFullInfo>(chat_info->group_id),
                            td::make_unique<TdOnGetChatFullInfoCallback>(this, chat_id, std::move(query)));
      case ChatInfo::Type::Supergroup:
        return send_request(make_object<td_api::getSupergroupFullInfo>(chat_info->supergroup_id),
                            td::make_unique<TdOnGetChatFullInfoCallback>(this, chat_id, std::move(query)));
      case ChatInfo::Type::Unknown:
      default:
        UNREACHABLE();
    }
  });
  return td::Status::OK();
}

td::Status Client::process_get_user_personal_chat_messages_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto limit = get_integer_arg(query.get(), "limit", 0, 0, 20);

  check_user(user_id, std::move(query), [this, user_id, limit](PromisedQueryPtr query) mutable {
    send_request(make_object<td_api::getPersonalChatHistory>(user_id, limit),
                 td::make_unique<TdOnGetMessagesCallback>(this, std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_set_chat_photo_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto photo = get_input_file(query.get(), "photo", true);
  if (photo == nullptr) {
    if (query->arg("photo").empty()) {
      return td::Status::Error(400, "There is no photo in the request");
    }
    return td::Status::Error(400, "Photo must be uploaded as an InputFile");
  }

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, photo = std::move(photo)](int64 chat_id, PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::setChatPhoto>(
                                chat_id, make_object<td_api::inputChatPhotoStatic>(std::move(photo))),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_delete_chat_photo_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::setChatPhoto>(chat_id, nullptr),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_set_chat_title_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto title = query->arg("title");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, title = title.str()](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::setChatTitle>(chat_id, title),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_set_chat_permissions_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  bool allow_legacy = false;
  auto use_independent_chat_permissions = to_bool(query->arg("use_independent_chat_permissions"));
  TRY_RESULT(permissions, get_chat_permissions(query.get(), allow_legacy, use_independent_chat_permissions));
  CHECK(!allow_legacy);

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, permissions = std::move(permissions)](int64 chat_id, PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::setChatPermissions>(chat_id, std::move(permissions)),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_set_chat_description_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto description = query->arg("description");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, description = description.str()](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::setChatDescription>(chat_id, description),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_pin_chat_message_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id");
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  auto disable_notification = to_bool(query->arg("disable_notification"));

  if (!business_connection_id.empty()) {
    check_business_connection_chat_id(
        business_connection_id.str(), chat_id.str(), std::move(query),
        [this, message_id](const BusinessConnection *business_connection, int64 chat_id, PromisedQueryPtr query) {
          send_request(
              make_object<td_api::setBusinessMessageIsPinned>(business_connection->id_, chat_id, message_id, true),
              td::make_unique<TdOnOkQueryCallback>(std::move(query)));
        });
    return td::Status::OK();
  }
  check_message(chat_id, message_id, false, AccessRights::Write, "message to pin", std::move(query),
                [this, disable_notification](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                  send_request(make_object<td_api::pinChatMessage>(chat_id, message_id, disable_notification, false),
                               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                });
  return td::Status::OK();
}

td::Status Client::process_unpin_chat_message_query(PromisedQueryPtr &query) {
  auto business_connection_id = query->arg("business_connection_id");
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());

  if (!business_connection_id.empty()) {
    check_business_connection_chat_id(
        business_connection_id.str(), chat_id.str(), std::move(query),
        [this, message_id](const BusinessConnection *business_connection, int64 chat_id, PromisedQueryPtr query) {
          send_request(
              make_object<td_api::setBusinessMessageIsPinned>(business_connection->id_, chat_id, message_id, false),
              td::make_unique<TdOnOkQueryCallback>(std::move(query)));
        });
    return td::Status::OK();
  }

  if (message_id == 0) {
    check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
      send_request(make_object<td_api::getChatPinnedMessage>(chat_id),
                   td::make_unique<TdOnGetChatPinnedMessageToUnpinCallback>(this, chat_id, std::move(query)));
    });
  } else {
    check_message(chat_id, message_id, false, AccessRights::Write, "message to unpin", std::move(query),
                  [this](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                    send_request(make_object<td_api::unpinChatMessage>(chat_id, message_id),
                                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                  });
  }
  return td::Status::OK();
}

td::Status Client::process_unpin_all_chat_messages_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::unpinAllChatMessages>(chat_id),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_approve_suggested_post_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  auto send_date = td::to_integer<int32>(query->arg("send_date"));

  check_message(chat_id, message_id, false, AccessRights::Write, "suggested post", std::move(query),
                [this, send_date](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                  send_request(make_object<td_api::approveSuggestedPost>(chat_id, message_id, send_date),
                               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                });
  return td::Status::OK();
}

td::Status Client::process_decline_suggested_post_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto message_id = get_message_id(query.get());
  auto comment = query->arg("comment");

  check_message(chat_id, message_id, false, AccessRights::Write, "suggested post", std::move(query),
                [this, comment = comment.str()](int64 chat_id, int64 message_id, PromisedQueryPtr query) {
                  send_request(make_object<td_api::declineSuggestedPost>(chat_id, message_id, comment),
                               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                });
  return td::Status::OK();
}

td::Status Client::process_set_chat_sticker_set_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto sticker_set_name = query->arg("sticker_set_name");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, sticker_set_name = sticker_set_name.str()](int64 chat_id, PromisedQueryPtr query) {
               if (get_chat_type(chat_id) != ChatType::Supergroup) {
                 return fail_query(400, "Bad Request: method is available only in supergroups", std::move(query));
               }

               resolve_sticker_set(
                   sticker_set_name, std::move(query), [this, chat_id](int64 sticker_set_id, PromisedQueryPtr query) {
                     auto chat_info = get_chat(chat_id);
                     CHECK(chat_info != nullptr);
                     CHECK(chat_info->type == ChatInfo::Type::Supergroup);
                     send_request(
                         make_object<td_api::setSupergroupStickerSet>(chat_info->supergroup_id, sticker_set_id),
                         td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                   });
             });
  return td::Status::OK();
}

td::Status Client::process_delete_chat_sticker_set_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    if (get_chat_type(chat_id) != ChatType::Supergroup) {
      return fail_query(400, "Bad Request: method is available only in supergroups", std::move(query));
    }

    auto chat_info = get_chat(chat_id);
    CHECK(chat_info != nullptr);
    CHECK(chat_info->type == ChatInfo::Type::Supergroup);
    send_request(make_object<td_api::setSupergroupStickerSet>(chat_info->supergroup_id, 0),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_get_forum_topic_icon_stickers_query(PromisedQueryPtr &query) {
  send_request(make_object<td_api::getForumTopicDefaultIcons>(),
               td::make_unique<TdOnGetStickersCallback>(this, std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_create_forum_topic_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto name = query->arg("name");
  int32 icon_color = get_integer_arg(query.get(), "icon_color", 0);
  auto icon_custom_emoji_id = td::to_integer<int64>(query->arg("icon_custom_emoji_id"));

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, name = name.str(), icon_color, icon_custom_emoji_id](int64 chat_id, PromisedQueryPtr query) {
               send_request(
                   make_object<td_api::createForumTopic>(
                       chat_id, name, false, make_object<td_api::forumTopicIcon>(icon_color, icon_custom_emoji_id)),
                   td::make_unique<TdOnGetForumTopicInfoCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_edit_forum_topic_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto forum_topic_id = get_forum_topic_id(query.get(), "message_thread_id");
  auto name = query->arg("name");
  auto edit_icon_custom_emoji_id = query->has_arg("icon_custom_emoji_id");
  auto icon_custom_emoji_id = td::to_integer<int64>(query->arg("icon_custom_emoji_id"));

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, forum_topic_id, name = name.str(), edit_icon_custom_emoji_id, icon_custom_emoji_id](
                 int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::editForumTopic>(chat_id, forum_topic_id, name,
                                                                edit_icon_custom_emoji_id, icon_custom_emoji_id),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_close_forum_topic_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto forum_topic_id = get_forum_topic_id(query.get(), "message_thread_id");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, forum_topic_id](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::toggleForumTopicIsClosed>(chat_id, forum_topic_id, true),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_reopen_forum_topic_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto forum_topic_id = get_forum_topic_id(query.get(), "message_thread_id");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, forum_topic_id](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::toggleForumTopicIsClosed>(chat_id, forum_topic_id, false),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_delete_forum_topic_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto forum_topic_id = get_forum_topic_id(query.get(), "message_thread_id");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, forum_topic_id](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::deleteForumTopic>(chat_id, forum_topic_id),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_unpin_all_forum_topic_messages_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto forum_topic_id = get_forum_topic_id(query.get(), "message_thread_id");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, forum_topic_id](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::unpinAllForumTopicMessages>(chat_id, forum_topic_id),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_edit_general_forum_topic_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto name = query->arg("name");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, name = name.str()](int64 chat_id, PromisedQueryPtr query) {
               send_request(make_object<td_api::editForumTopic>(chat_id, GENERAL_FORUM_TOPIC_ID, name, false, 0),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_close_general_forum_topic_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::toggleForumTopicIsClosed>(chat_id, GENERAL_FORUM_TOPIC_ID, true),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_reopen_general_forum_topic_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::toggleForumTopicIsClosed>(chat_id, GENERAL_FORUM_TOPIC_ID, false),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_hide_general_forum_topic_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::toggleGeneralForumTopicIsHidden>(chat_id, true),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_unhide_general_forum_topic_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::toggleGeneralForumTopicIsHidden>(chat_id, false),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_unpin_all_general_forum_topic_messages_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::unpinAllForumTopicMessages>(chat_id, GENERAL_FORUM_TOPIC_ID),
                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_get_chat_member_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));

  check_chat(chat_id, user_id == my_id_ ? AccessRights::Read : AccessRights::ReadMembers, std::move(query),
             [this, user_id](int64 chat_id, PromisedQueryPtr query) {
               get_chat_member(chat_id, user_id, std::move(query),
                               [this, chat_type = get_chat_type(chat_id)](object_ptr<td_api::chatMember> &&chat_member,
                                                                          PromisedQueryPtr query) {
                                 answer_query(JsonChatMember(chat_member.get(), chat_type, this), std::move(query));
                               });
             });
  return td::Status::OK();
}

td::Status Client::process_get_chat_administrators_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::ReadMembers, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    auto chat_info = get_chat(chat_id);
    CHECK(chat_info != nullptr);
    switch (chat_info->type) {
      case ChatInfo::Type::Private:
        return fail_query(400, "Bad Request: there are no administrators in the private chat", std::move(query));
      case ChatInfo::Type::Group:
        return send_request(make_object<td_api::getBasicGroupFullInfo>(chat_info->group_id),
                            td::make_unique<TdOnGetGroupMembersCallback>(this, true, std::move(query)));
      case ChatInfo::Type::Supergroup:
        return send_request(
            make_object<td_api::getSupergroupMembers>(
                chat_info->supergroup_id, make_object<td_api::supergroupMembersFilterAdministrators>(), 0, 100),
            td::make_unique<TdOnGetSupergroupMembersCallback>(this, get_chat_type(chat_id), std::move(query)));
      case ChatInfo::Type::Unknown:
      default:
        UNREACHABLE();
    }
  });
  return td::Status::OK();
}

td::Status Client::process_get_chat_member_count_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::ReadMembers, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    auto chat_info = get_chat(chat_id);
    CHECK(chat_info != nullptr);
    switch (chat_info->type) {
      case ChatInfo::Type::Private:
        return answer_query(td::VirtuallyJsonableInt(1 + (chat_info->user_id != my_id_)), std::move(query));
      case ChatInfo::Type::Group: {
        auto group_info = get_group_info(chat_info->group_id);
        CHECK(group_info != nullptr);
        return answer_query(td::VirtuallyJsonableInt(group_info->member_count), std::move(query));
      }
      case ChatInfo::Type::Supergroup:
        return send_request(make_object<td_api::getSupergroupFullInfo>(chat_info->supergroup_id),
                            td::make_unique<TdOnGetSupergroupMemberCountCallback>(std::move(query)));
      case ChatInfo::Type::Unknown:
      default:
        UNREACHABLE();
    }
  });
  return td::Status::OK();
}

td::Status Client::process_leave_chat_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");

  check_chat(chat_id, AccessRights::Read, std::move(query), [this](int64 chat_id, PromisedQueryPtr query) {
    send_request(make_object<td_api::leaveChat>(chat_id), td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  });
  return td::Status::OK();
}

td::Status Client::process_promote_chat_member_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto can_manage_chat = to_bool(query->arg("can_manage_chat"));
  auto can_change_info = to_bool(query->arg("can_change_info"));
  auto can_post_messages = to_bool(query->arg("can_post_messages"));
  auto can_edit_messages = to_bool(query->arg("can_edit_messages"));
  auto can_delete_messages = to_bool(query->arg("can_delete_messages"));
  auto can_invite_users = to_bool(query->arg("can_invite_users"));
  auto can_restrict_members = to_bool(query->arg("can_restrict_members"));
  auto can_pin_messages = to_bool(query->arg("can_pin_messages"));
  auto can_manage_topics = to_bool(query->arg("can_manage_topics"));
  auto can_promote_members = to_bool(query->arg("can_promote_members"));
  auto can_manage_video_chats =
      to_bool(query->arg("can_manage_voice_chats")) || to_bool(query->arg("can_manage_video_chats"));
  auto can_post_stories = to_bool(query->arg("can_post_stories"));
  auto can_edit_stories = to_bool(query->arg("can_edit_stories"));
  auto can_delete_stories = to_bool(query->arg("can_delete_stories"));
  auto can_manage_direct_messages = to_bool(query->arg("can_manage_direct_messages"));
  auto can_manage_tags = to_bool(query->arg("can_manage_tags"));
  auto can_send_welcome_messages = to_bool(query->arg("can_send_welcome_messages"));
  auto is_anonymous = to_bool(query->arg("is_anonymous"));
  auto is_promotion = can_manage_chat || can_change_info || can_post_messages || can_edit_messages ||
                      can_delete_messages || can_invite_users || can_restrict_members || can_pin_messages ||
                      can_manage_topics || can_promote_members || can_manage_video_chats || can_post_stories ||
                      can_edit_stories || can_delete_stories || can_manage_direct_messages || can_manage_tags ||
                      can_send_welcome_messages || is_anonymous;
  auto status = make_object<td_api::chatMemberStatusAdministrator>(
      true, make_object<td_api::chatAdministratorRights>(
                can_manage_chat, can_change_info, can_post_messages, can_edit_messages, can_delete_messages,
                can_invite_users, can_restrict_members, can_pin_messages, can_manage_topics, can_promote_members,
                can_manage_video_chats, can_post_stories, can_edit_stories, can_delete_stories,
                can_manage_direct_messages, can_manage_tags, can_send_welcome_messages, is_anonymous));
  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, user_id, status = std::move(status), is_promotion](int64 chat_id, PromisedQueryPtr query) mutable {
               auto chat_info = get_chat(chat_id);
               CHECK(chat_info != nullptr);
               if (chat_info->type != ChatInfo::Type::Supergroup) {
                 return fail_query(400, "Bad Request: method is available only in supergroup and channel chats",
                                   std::move(query));
               }
               auto supergroup_info = get_supergroup_info(chat_info->supergroup_id);
               if (supergroup_info != nullptr && !supergroup_info->is_supergroup && is_promotion &&
                   !query->has_arg("can_restrict_members")) {
                 status->rights_->can_restrict_members_ = true;
               }

               send_request(make_object<td_api::setChatMemberStatus>(
                                chat_id, make_object<td_api::messageSenderUser>(user_id), std::move(status)),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_set_chat_administrator_custom_title_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));

  check_chat(chat_id, AccessRights::Write, std::move(query), [this, user_id](int64 chat_id, PromisedQueryPtr query) {
    auto chat_type = get_chat_type(chat_id);
    if (chat_type != ChatType::Group && chat_type != ChatType::Supergroup) {
      return fail_query(400, "Bad Request: method is available only in groups and supergroups", std::move(query));
    }

    get_chat_member(chat_id, user_id, std::move(query),
                    [this, chat_id, user_id](object_ptr<td_api::chatMember> &&chat_member, PromisedQueryPtr query) {
                      if (chat_member->status_->get_id() == td_api::chatMemberStatusCreator::ID) {
                        return fail_query(400, "Bad Request: only the owner can edit their custom title",
                                          std::move(query));
                      }
                      if (chat_member->status_->get_id() != td_api::chatMemberStatusAdministrator::ID) {
                        return fail_query(400, "Bad Request: user is not an administrator", std::move(query));
                      }
                      auto administrator = move_object_as<td_api::chatMemberStatusAdministrator>(chat_member->status_);
                      if (!administrator->can_be_edited_) {
                        return fail_query(400, "Bad Request: not enough rights to change custom title of the user",
                                          std::move(query));
                      }

                      auto custom_title = query->arg("custom_title").str();
                      send_request(make_object<td_api::setChatMemberTag>(chat_id, user_id, custom_title),
                                   td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                    });
  });
  return td::Status::OK();
}

td::Status Client::process_set_chat_member_tag_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));

  check_chat(chat_id, AccessRights::Write, std::move(query), [this, user_id](int64 chat_id, PromisedQueryPtr query) {
    auto chat_type = get_chat_type(chat_id);
    if (chat_type != ChatType::Group && chat_type != ChatType::Supergroup) {
      return fail_query(400, "Bad Request: method is available only in groups and supergroups", std::move(query));
    }

    check_user_no_fail(user_id, std::move(query), [this, chat_id, user_id](PromisedQueryPtr query) {
      auto tag = query->arg("tag").str();
      send_request(make_object<td_api::setChatMemberTag>(chat_id, user_id, tag),
                   td::make_unique<TdOnOkQueryCallback>(std::move(query)));
    });
  });
  return td::Status::OK();
}

td::Status Client::process_ban_chat_member_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));
  int32 until_date = get_integer_arg(query.get(), "until_date", 0);
  auto revoke_messages = to_bool(query->arg("revoke_messages"));

  check_chat(
      chat_id, AccessRights::Write, std::move(query),
      [this, user_id, until_date, revoke_messages](int64 chat_id, PromisedQueryPtr query) {
        check_user_no_fail(
            user_id, std::move(query), [this, chat_id, user_id, until_date, revoke_messages](PromisedQueryPtr query) {
              send_request(make_object<td_api::banChatMember>(chat_id, make_object<td_api::messageSenderUser>(user_id),
                                                              until_date, revoke_messages),
                           td::make_unique<TdOnOkQueryCallback>(std::move(query)));
            });
      });
  return td::Status::OK();
}

td::Status Client::process_restrict_chat_member_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));
  int32 until_date = get_integer_arg(query.get(), "until_date", 0);
  bool allow_legacy = true;
  auto use_independent_chat_permissions = to_bool(query->arg("use_independent_chat_permissions"));
  TRY_RESULT(permissions, get_chat_permissions(query.get(), allow_legacy, use_independent_chat_permissions));

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, user_id, until_date, is_legacy = allow_legacy, permissions = std::move(permissions)](
                 int64 chat_id, PromisedQueryPtr query) mutable {
               if (get_chat_type(chat_id) != ChatType::Supergroup) {
                 return fail_query(400, "Bad Request: method is available only in supergroups", std::move(query));
               }

               get_chat_member(
                   chat_id, user_id, std::move(query),
                   [this, chat_id, user_id, until_date, is_legacy, permissions = std::move(permissions)](
                       object_ptr<td_api::chatMember> &&chat_member, PromisedQueryPtr query) mutable {
                     if (is_legacy && chat_member->status_->get_id() == td_api::chatMemberStatusRestricted::ID) {
                       auto restricted =
                           static_cast<const td_api::chatMemberStatusRestricted *>(chat_member->status_.get());
                       auto *old_permissions = restricted->permissions_.get();
                       permissions->can_send_polls_ = old_permissions->can_send_polls_;
                       permissions->can_change_info_ = old_permissions->can_change_info_;
                       permissions->can_invite_users_ = old_permissions->can_invite_users_;
                       permissions->can_pin_messages_ = old_permissions->can_pin_messages_;
                       permissions->can_create_topics_ = old_permissions->can_create_topics_;
                     }

                     send_request(make_object<td_api::setChatMemberStatus>(
                                      chat_id, make_object<td_api::messageSenderUser>(user_id),
                                      make_object<td_api::chatMemberStatusRestricted>(
                                          is_chat_member(chat_member->status_), until_date, std::move(permissions))),
                                  td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                   });
             });
  return td::Status::OK();
}

td::Status Client::process_unban_chat_member_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto only_if_banned = to_bool(query->arg("only_if_banned"));

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, user_id, only_if_banned](int64 chat_id, PromisedQueryPtr query) {
               auto chat_info = get_chat(chat_id);
               CHECK(chat_info != nullptr);
               if (chat_info->type != ChatInfo::Type::Supergroup) {
                 return fail_query(400, "Bad Request: method is available only in supergroup and channel chats",
                                   std::move(query));
               }

               if (only_if_banned) {
                 get_chat_member(
                     chat_id, user_id, std::move(query),
                     [this, chat_id, user_id](object_ptr<td_api::chatMember> &&chat_member, PromisedQueryPtr query) {
                       if (chat_member->status_->get_id() != td_api::chatMemberStatusBanned::ID) {
                         return answer_query(td::JsonTrue(), std::move(query));
                       }

                       send_request(make_object<td_api::setChatMemberStatus>(
                                        chat_id, make_object<td_api::messageSenderUser>(user_id),
                                        make_object<td_api::chatMemberStatusLeft>()),
                                    td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                     });
               } else {
                 check_user_no_fail(user_id, std::move(query), [this, chat_id, user_id](PromisedQueryPtr query) {
                   send_request(make_object<td_api::setChatMemberStatus>(
                                    chat_id, make_object<td_api::messageSenderUser>(user_id),
                                    make_object<td_api::chatMemberStatusLeft>()),
                                td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                 });
               }
             });
  return td::Status::OK();
}

td::Status Client::process_ban_chat_sender_chat_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto sender_chat_id = query->arg("sender_chat_id");
  int32 until_date = get_integer_arg(query.get(), "until_date", 0);

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, sender_chat_id = sender_chat_id.str(), until_date](int64 chat_id, PromisedQueryPtr query) {
               check_chat_no_fail(sender_chat_id, std::move(query),
                                  [this, chat_id, until_date](int64 sender_chat_id, PromisedQueryPtr query) {
                                    send_request(make_object<td_api::banChatMember>(
                                                     chat_id, make_object<td_api::messageSenderChat>(sender_chat_id),
                                                     until_date, false),
                                                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                                  });
             });
  return td::Status::OK();
}

td::Status Client::process_unban_chat_sender_chat_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  auto sender_chat_id = query->arg("sender_chat_id");

  check_chat(chat_id, AccessRights::Write, std::move(query),
             [this, sender_chat_id = sender_chat_id.str()](int64 chat_id, PromisedQueryPtr query) {
               check_chat_no_fail(sender_chat_id, std::move(query),
                                  [this, chat_id](int64 sender_chat_id, PromisedQueryPtr query) {
                                    send_request(make_object<td_api::setChatMemberStatus>(
                                                     chat_id, make_object<td_api::messageSenderChat>(sender_chat_id),
                                                     make_object<td_api::chatMemberStatusLeft>()),
                                                 td::make_unique<TdOnOkQueryCallback>(std::move(query)));
                                  });
             });
  return td::Status::OK();
}

td::Status Client::process_approve_chat_join_request_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));

  check_chat(chat_id, AccessRights::Write, std::move(query), [this, user_id](int64 chat_id, PromisedQueryPtr query) {
    check_user_no_fail(user_id, std::move(query), [this, chat_id, user_id](PromisedQueryPtr query) {
      send_request(make_object<td_api::processChatJoinRequest>(chat_id, user_id, true),
                   td::make_unique<TdOnOkQueryCallback>(std::move(query)));
    });
  });
  return td::Status::OK();
}

td::Status Client::process_decline_chat_join_request_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));

  check_chat(chat_id, AccessRights::Write, std::move(query), [this, user_id](int64 chat_id, PromisedQueryPtr query) {
    check_user_no_fail(user_id, std::move(query), [this, chat_id, user_id](PromisedQueryPtr query) {
      send_request(make_object<td_api::processChatJoinRequest>(chat_id, user_id, false),
                   td::make_unique<TdOnOkQueryCallback>(std::move(query)));
    });
  });
  return td::Status::OK();
}

td::Status Client::process_get_user_chat_boosts_query(PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(user_id, get_user_id(query.get()));

  check_chat(chat_id, AccessRights::Write, std::move(query), [this, user_id](int64 chat_id, PromisedQueryPtr query) {
    check_user_no_fail(user_id, std::move(query), [this, chat_id, user_id](PromisedQueryPtr query) {
      send_request(make_object<td_api::getUserChatBoosts>(chat_id, user_id),
                   td::make_unique<TdOnGetUserChatBoostsCallback>(this, std::move(query)));
    });
  });
  return td::Status::OK();
}

td::Status Client::process_get_sticker_set_query(PromisedQueryPtr &query) {
  auto name = query->arg("name");
  if (td::trim(to_lower(name)) == to_lower(GREAT_MINDS_SET_NAME)) {
    send_request(make_object<td_api::getStickerSet>(GREAT_MINDS_SET_ID),
                 td::make_unique<TdOnReturnStickerSetCallback>(this, true, std::move(query)));
  } else {
    send_request(make_object<td_api::searchStickerSet>(name.str(), false),
                 td::make_unique<TdOnReturnStickerSetCallback>(this, true, std::move(query)));
  }
  return td::Status::OK();
}

td::Status Client::process_get_custom_emoji_stickers_query(PromisedQueryPtr &query) {
  TRY_RESULT(custom_emoji_ids_json, get_required_string_arg(query.get(), "custom_emoji_ids"));

  LOG(INFO) << "Parsing JSON object: " << custom_emoji_ids_json;
  auto r_value = json_decode(custom_emoji_ids_json);
  if (r_value.is_error()) {
    return td::Status::Error(400, "Can't parse custom emoji identifiers JSON object");
  }
  auto value = r_value.move_as_ok();
  if (value.type() != td::JsonValue::Type::Array) {
    return td::Status::Error(400, "Expected an Array of custom emoji identifiers");
  }

  td::vector<int64> custom_emoji_ids;
  for (auto &custom_emoji_id : value.get_array()) {
    if (custom_emoji_id.type() != td::JsonValue::Type::String) {
      return td::Status::Error(400, "Custom emoji identifier must be of type String");
    }
    auto parsed_id = td::to_integer_safe<int64>(custom_emoji_id.get_string());
    if (parsed_id.is_error()) {
      return td::Status::Error(400, "Invalid custom emoji identifier specified");
    }
    custom_emoji_ids.push_back(parsed_id.ok());
  }

  send_request(make_object<td_api::getCustomEmojiStickers>(std::move(custom_emoji_ids)),
               td::make_unique<TdOnGetStickersCallback>(this, std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_upload_sticker_file_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  object_ptr<td_api::StickerFormat> sticker_format;
  object_ptr<td_api::InputFile> sticker;
  if (query->has_arg("sticker") || query->file("sticker") != nullptr) {
    TRY_RESULT_ASSIGN(sticker_format, get_sticker_format(query->arg("sticker_format")));
    sticker = get_input_file(query.get(), "sticker", true);
  } else {
    sticker_format = make_object<td_api::stickerFormatWebp>();
    sticker = get_input_file(query.get(), "png_sticker", true);
  }

  check_user(user_id, std::move(query),
             [this, user_id, sticker_format = std::move(sticker_format),
              sticker = std::move(sticker)](PromisedQueryPtr query) mutable {
               send_request(
                   make_object<td_api::uploadStickerFile>(user_id, std::move(sticker_format), std::move(sticker)),
                   td::make_unique<TdOnReturnFileCallback>(this, std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_create_new_sticker_set_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto name = query->arg("name");
  auto title = query->arg("title");
  auto needs_repainting = to_bool(query->arg("needs_repainting"));
  TRY_RESULT(stickers, get_new_stickers(query.get()));

  TRY_RESULT(sticker_type, get_sticker_type(query->arg("sticker_type")));
  if (to_bool(query->arg("contains_masks"))) {
    sticker_type = make_object<td_api::stickerTypeMask>();
  }

  check_user(user_id, std::move(query),
             [this, user_id, title, name, sticker_type = std::move(sticker_type), needs_repainting,
              stickers = std::move(stickers)](PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::createNewStickerSet>(user_id, title.str(), name.str(),
                                                                     std::move(sticker_type), needs_repainting,
                                                                     std::move(stickers), PSTRING() << "bot" << my_id_),
                            td::make_unique<TdOnReturnStickerSetCallback>(this, false, std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_add_sticker_to_set_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto name = query->arg("name");
  TRY_RESULT(sticker, get_new_sticker(query.get()));

  check_user(user_id, std::move(query),
             [this, user_id, name, sticker = std::move(sticker)](PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::addStickerToSet>(user_id, name.str(), std::move(sticker)),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_replace_sticker_in_set_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto name = query->arg("name");
  TRY_RESULT(input_file, get_sticker_input_file(query.get(), "old_sticker"));
  TRY_RESULT(sticker, get_new_sticker(query.get()));

  check_user(user_id, std::move(query),
             [this, user_id, name, input_file = std::move(input_file),
              sticker = std::move(sticker)](PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::replaceStickerInSet>(user_id, name.str(), std::move(input_file),
                                                                     std::move(sticker)),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_set_sticker_set_title_query(PromisedQueryPtr &query) {
  auto name = query->arg("name");
  auto title = query->arg("title");
  send_request(make_object<td_api::setStickerSetTitle>(name.str(), title.str()),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_set_sticker_set_thumbnail_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  auto name = query->arg("name");
  auto thumbnail = get_input_file(query.get(), "thumbnail");
  if (thumbnail == nullptr) {
    thumbnail = get_input_file(query.get(), "thumb");
  }
  td::Slice sticker_format_str = query->arg("format");
  if (sticker_format_str.empty()) {
    sticker_format_str = td::Slice("auto");
  }
  TRY_RESULT(sticker_format, get_sticker_format(sticker_format_str));
  check_user(user_id, std::move(query),
             [this, user_id, name, thumbnail = std::move(thumbnail),
              sticker_format = std::move(sticker_format)](PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::setStickerSetThumbnail>(user_id, name.str(), std::move(thumbnail),
                                                                        std::move(sticker_format)),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_set_custom_emoji_sticker_set_thumbnail_query(PromisedQueryPtr &query) {
  auto name = query->arg("name");
  auto custom_emoji_id = td::to_integer<int64>(query->arg("custom_emoji_id"));
  send_request(make_object<td_api::setCustomEmojiStickerSetThumbnail>(name.str(), custom_emoji_id),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_delete_sticker_set_query(PromisedQueryPtr &query) {
  auto name = query->arg("name");
  send_request(make_object<td_api::deleteStickerSet>(name.str()),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_set_sticker_position_in_set_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_file, get_sticker_input_file(query.get()));
  int32 position = get_integer_arg(query.get(), "position", -1);

  send_request(make_object<td_api::setStickerPositionInSet>(std::move(input_file), position),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_delete_sticker_from_set_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_file, get_sticker_input_file(query.get()));

  send_request(make_object<td_api::removeStickerFromSet>(std::move(input_file)),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_set_sticker_emoji_list_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_file, get_sticker_input_file(query.get()));
  TRY_RESULT(emojis, get_sticker_emojis(query->arg("emoji_list")));

  send_request(make_object<td_api::setStickerEmojis>(std::move(input_file), emojis),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_set_sticker_keywords_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_file, get_sticker_input_file(query.get()));
  td::vector<td::string> input_keywords;
  if (query->has_arg("keywords")) {
    auto r_value = json_decode(query->arg("keywords"));
    if (r_value.is_error()) {
      LOG(INFO) << "Can't parse JSON object: " << r_value.error();
      return td::Status::Error(400, "Can't parse keywords JSON object");
    }
    auto value = r_value.move_as_ok();

    if (value.type() != td::JsonValue::Type::Array) {
      return td::Status::Error(400, "Field \"keywords\" must be an Array");
    }
    for (auto &keyword : value.get_array()) {
      if (keyword.type() != td::JsonValue::Type::String) {
        return td::Status::Error(400, "keyword must be a string");
      }
      input_keywords.push_back(keyword.get_string().str());
    }
  }

  send_request(make_object<td_api::setStickerKeywords>(std::move(input_file), std::move(input_keywords)),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_set_sticker_mask_position_query(PromisedQueryPtr &query) {
  TRY_RESULT(input_file, get_sticker_input_file(query.get()));
  TRY_RESULT(mask_position, get_mask_position(query.get(), "mask_position"));

  send_request(make_object<td_api::setStickerMaskPosition>(std::move(input_file), std::move(mask_position)),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_set_passport_data_errors_query(PromisedQueryPtr &query) {
  TRY_RESULT(user_id, get_user_id(query.get()));
  TRY_RESULT(passport_element_errors, get_passport_element_errors(query.get()));

  check_user(user_id, std::move(query),
             [this, user_id, errors = std::move(passport_element_errors)](PromisedQueryPtr query) mutable {
               send_request(make_object<td_api::setPassportElementErrors>(user_id, std::move(errors)),
                            td::make_unique<TdOnOkQueryCallback>(std::move(query)));
             });
  return td::Status::OK();
}

td::Status Client::process_send_custom_request_query(PromisedQueryPtr &query) {
  TRY_RESULT(method, get_required_string_arg(query.get(), "method"));
  auto parameters = query->arg("parameters");
  send_request(make_object<td_api::sendCustomRequest>(method.str(), parameters.str()),
               td::make_unique<TdOnSendCustomRequestCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_answer_custom_query_query(PromisedQueryPtr &query) {
  auto custom_query_id = td::to_integer<int64>(query->arg("custom_query_id"));
  auto data = query->arg("data");
  send_request(make_object<td_api::answerCustomQuery>(custom_query_id, data.str()),
               td::make_unique<TdOnOkQueryCallback>(std::move(query)));
  return td::Status::OK();
}

td::Status Client::process_get_updates_query(PromisedQueryPtr &query) {
  if (!webhook_url_.empty() || webhook_set_query_ || active_webhook_set_query_) {
    fail_query_conflict(
        "Conflict: can't use getUpdates method while webhook is active; use deleteWebhook to delete the webhook first",
        std::move(query));
    return td::Status::OK();
  }
  int32 offset = get_integer_arg(query.get(), "offset", 0);
  int32 limit = get_integer_arg(query.get(), "limit", 100, 1, 100);
  int32 timeout = get_integer_arg(query.get(), "timeout", 0, 0, LONG_POLL_MAX_TIMEOUT);

  update_allowed_update_types(query.get());

  auto now = td::Time::now_cached();
  if (offset == previous_get_updates_offset_ && timeout < 3 && now < previous_get_updates_start_time_ + 3.0) {
    timeout = 3;
  }
  if (offset == previous_get_updates_offset_ && now < previous_get_updates_start_time_ + 0.5) {
    limit = 1;
  }
  previous_get_updates_offset_ = offset;
  previous_get_updates_start_time_ = now;
  do_get_updates(offset, limit, timeout, std::move(query));
  return td::Status::OK();
}

td::Status Client::process_set_webhook_query(PromisedQueryPtr &query) {
  td::Slice new_url;
  if (query->method() == "setwebhook") {
    new_url = query->arg("url");
  }

  auto now = td::Time::now_cached();
  if (!new_url.empty() && !query->is_internal()) {
    if (now < next_allowed_set_webhook_time_) {
      query->set_retry_after_error(1);
      return td::Status::OK();
    }
    next_allowed_set_webhook_time_ = now + 1;
  }

  // do not send warning just after webhook was deleted or set
  next_bot_updates_warning_time_ = td::max(next_bot_updates_warning_time_, now + BOT_UPDATES_WARNING_DELAY);

  bool new_has_certificate = new_url.empty() ? false
                                             : (get_webhook_certificate(query.get()) != nullptr ||
                                                (query->is_internal() && query->arg("certificate") == "previous"));
  int32 new_max_connections = new_url.empty() ? 0 : get_webhook_max_connections(query.get());
  td::Slice new_ip_address = new_url.empty() ? td::Slice() : query->arg("ip_address");
  bool new_fix_ip_address = new_url.empty() ? false : get_webhook_fix_ip_address(query.get());
  td::Slice new_secret_token = new_url.empty() ? td::Slice() : query->arg("secret_token");
  bool drop_pending_updates = to_bool(query->arg("drop_pending_updates"));
  if (webhook_set_query_) {
    // already updating webhook. Cancel previous request
    fail_query_conflict("Conflict: terminated by other setWebhook", std::move(webhook_set_query_));
  } else if (active_webhook_set_query_) {
    query->set_retry_after_error(1);
    return td::Status::OK();
  } else if (webhook_url_ == new_url && !has_webhook_certificate_ && !new_has_certificate &&
             new_max_connections == webhook_max_connections_ && new_fix_ip_address == webhook_fix_ip_address_ &&
             new_secret_token == webhook_secret_token_ &&
             (!new_fix_ip_address || new_ip_address == webhook_ip_address_) && !drop_pending_updates) {
    if (update_allowed_update_types(query.get())) {
      save_webhook();
    } else if (now > next_webhook_is_not_modified_warning_time_) {
      next_webhook_is_not_modified_warning_time_ = now + 300;
      LOG(WARNING) << "Webhook is not modified: \"" << new_url << '"';
    }
    answer_query(td::JsonTrue(), std::move(query),
                 new_url.empty() ? td::Slice("Webhook is already deleted") : td::Slice("Webhook is already set"));
    return td::Status::OK();
  }

  if (now > next_set_webhook_logging_time_ || webhook_url_ != new_url) {
    next_set_webhook_logging_time_ = now + 300;
    LOG(WARNING) << "Set webhook to " << new_url << ", max_connections = " << new_max_connections
                 << ", IP address = " << new_ip_address << ", drop_pending_updates = " << drop_pending_updates;
  }

  if (!new_url.empty()) {
    abort_long_poll(true);
  }

  webhook_generation_++;
  // need to close old webhook first
  if (!webhook_url_.empty()) {
    if (!webhook_id_.empty()) {
      send_closure_later(std::move(webhook_id_), &WebhookActor::close);
    }

    // wait for webhook_close callback
    webhook_query_type_ = WebhookQueryType::Cancel;
    CHECK(!active_webhook_set_query_);
    webhook_set_query_ = std::move(query);
    return td::Status::OK();
  }
  do_set_webhook(std::move(query), false);
  return td::Status::OK();
}

td::Status Client::process_get_webhook_info_query(PromisedQueryPtr &query) {
  update_last_synchronization_error_date();
  answer_query(JsonWebhookInfo(this), std::move(query));
  return td::Status::OK();
}

td::Status Client::process_get_file_query(PromisedQueryPtr &query) {
  td::string file_id = query->arg("file_id").str();
  check_remote_file_id(file_id, std::move(query), [this](object_ptr<td_api::file> file, PromisedQueryPtr query) {
    do_get_file(std::move(file), std::move(query));
  });
  return td::Status::OK();
}

void Client::do_get_file(object_ptr<td_api::file> file, PromisedQueryPtr query) {
  if (!parameters_->local_mode_ &&
      td::max(file->expected_size_, file->local_->downloaded_size_) > MAX_DOWNLOAD_FILE_SIZE) {  // speculative check
    return fail_query(400, "Bad Request: file is too big", std::move(query));
  }

  auto file_id = file->id_;
  file_download_listeners_[file_id].push_back(std::move(query));
  send_request(make_object<td_api::downloadFile>(file_id, 1, 0, 0, false),
               td::make_unique<TdOnDownloadFileCallback>(this, file_id));
}

bool Client::is_file_being_downloaded(int32 file_id) const {
  return file_download_listeners_.count(file_id) > 0;
}

void Client::on_file_download(int32 file_id, td::Result<object_ptr<td_api::file>> r_file) {
  auto it = file_download_listeners_.find(file_id);
  if (it == file_download_listeners_.end()) {
    return;
  }
  auto queries = std::move(it->second);
  file_download_listeners_.erase(it);
  download_started_file_ids_.erase(file_id);
  for (auto &query : queries) {
    if (r_file.is_error()) {
      const auto &error = r_file.error();
      fail_query_with_error(std::move(query), error.code(), error.public_message());
    } else {
      answer_query(JsonFile(r_file.ok().get(), this, true), std::move(query));
    }
  }
}

void Client::return_stickers(object_ptr<td_api::stickers> stickers, PromisedQueryPtr query) {
  answer_query(JsonStickers(stickers->stickers_, this), std::move(query));
}

void Client::return_gifts(td::vector<object_ptr<td_api::gift>> gifts, PromisedQueryPtr query) {
  answer_query(JsonGifts(gifts, this), std::move(query));
}

void Client::return_received_gifts(object_ptr<td_api::receivedGifts> gifts, bool can_be_managed,
                                   PromisedQueryPtr query) {
  answer_query(JsonReceivedGifts(gifts.get(), can_be_managed, this), std::move(query));
}

void Client::return_star_transactions(object_ptr<td_api::starTransactions> transactions, PromisedQueryPtr query) {
  answer_query(JsonStarTransactions(transactions.get(), this), std::move(query));
}

void Client::return_chat_full_info(int64 chat_id, int64 pinned_message_id, PromisedQueryPtr query) {
  answer_query(JsonChat(chat_id, this, true, pinned_message_id), std::move(query));
}

void Client::webhook_verified(td::string cached_ip_address) {
  if (get_link_token() != webhook_generation_) {
    return;
  }
  bool need_save = webhook_set_query_ || cached_ip_address != webhook_ip_address_;
  webhook_ip_address_ = cached_ip_address;
  if (webhook_set_query_) {
    LOG(WARNING) << "Webhook verified";
    answer_query(td::JsonTrue(), std::move(webhook_set_query_), "Webhook was set");
  }
  if (need_save) {
    save_webhook();
  }
}

void Client::save_webhook() const {
  td::string value;
  if (has_webhook_certificate_) {
    value += "cert/";
  }
  value += PSTRING() << "#maxc" << webhook_max_connections_ << '/';
  if (!webhook_ip_address_.empty()) {
    value += PSTRING() << "#ip" << webhook_ip_address_ << '/';
  }
  if (webhook_fix_ip_address_) {
    value += "#fix_ip/";
  }
  if (!webhook_secret_token_.empty()) {
    value += PSTRING() << "#secret" << webhook_secret_token_ << '/';
  }
  if (allowed_update_types_ != DEFAULT_ALLOWED_UPDATE_TYPES) {
    value += PSTRING() << "#allow" << allowed_update_types_ << '/';
  }
  value += webhook_url_;
  LOG(INFO) << "Save webhook " << value;
  parameters_->shared_data_->webhook_db_->set(bot_token_with_dc_, value);
}

void Client::webhook_success() {
  next_bot_updates_warning_time_ = td::Time::now() + BOT_UPDATES_WARNING_DELAY;
  if (was_bot_updates_warning_) {
    send_request(make_object<td_api::setBotUpdatesStatus>(0, ""), td::make_unique<TdOnOkCallback>());
    was_bot_updates_warning_ = false;
  }
}

void Client::webhook_error(td::Status status) {
  CHECK(status.is_error());
  last_webhook_error_date_ = get_unix_time();
  last_webhook_error_ = std::move(status);

  auto pending_update_count = get_pending_update_count();
  if (pending_update_count >= MIN_PENDING_UPDATES_WARNING && td::Time::now() > next_bot_updates_warning_time_) {
    send_request(make_object<td_api::setBotUpdatesStatus>(td::narrow_cast<int32>(pending_update_count),
                                                          "Webhook error. " + last_webhook_error_.message().str()),
                 td::make_unique<TdOnOkCallback>());
    next_bot_updates_warning_time_ = td::Time::now_cached() + BOT_UPDATES_WARNING_DELAY;
    was_bot_updates_warning_ = true;
  }
}

void Client::webhook_closed(td::Status status) {
  if (has_webhook_certificate_) {
    td::Scheduler::instance()->run_on_scheduler(SharedData::get_webhook_certificate_scheduler_id(),
                                                [actor_id = actor_id(this), path = get_webhook_certificate_path(),
                                                 status = std::move(status)](td::Unit) mutable {
                                                  LOG(INFO) << "Unlink certificate " << path;
                                                  td::unlink(path).ignore();
                                                  send_closure(actor_id, &Client::on_webhook_closed, std::move(status));
                                                });
    return;
  }
  on_webhook_closed(std::move(status));
}

void Client::on_webhook_closed(td::Status status) {
  LOG(WARNING) << "Webhook closed: " << status
               << ", webhook_query_type = " << (webhook_query_type_ == WebhookQueryType::Verify ? "verify" : "change");
  webhook_id_.release();
  webhook_url_ = td::string();
  has_webhook_certificate_ = false;
  webhook_max_connections_ = 0;
  webhook_ip_address_ = td::string();
  webhook_fix_ip_address_ = false;
  webhook_secret_token_ = td::string();
  webhook_set_time_ = td::Time::now();
  last_webhook_error_date_ = 0;
  last_webhook_error_ = td::Status::OK();
  parameters_->shared_data_->webhook_db_->erase(bot_token_with_dc_);

  if (webhook_set_query_) {
    if (webhook_query_type_ == WebhookQueryType::Verify) {
      fail_query(400, PSLICE() << "Bad Request: bad webhook: " << status.message(), std::move(webhook_set_query_));
    } else {
      do_set_webhook(std::move(webhook_set_query_), true);
    }
  }
}

void Client::hangup_shared() {
  webhook_closed(td::Status::Error("Unknown"));
}

td::string Client::get_webhook_certificate_path() const {
  return dir_ + "cert.pem";
}

const td::HttpFile *Client::get_webhook_certificate(const Query *query) const {
  auto file = query->file("certificate");
  if (file == nullptr) {
    auto attach_name = query->arg("certificate");
    td::Slice attach_protocol{"attach://"};
    if (td::begins_with(attach_name, attach_protocol)) {
      file = query->file(attach_name.substr(attach_protocol.size()));
    }
  }
  return file;
}

td::int32 Client::get_webhook_max_connections(const Query *query) const {
  auto default_value = parameters_->default_max_webhook_connections_;
  auto max_value = parameters_->local_mode_ ? 100000 : 100;
  return get_integer_arg(query, "max_connections", default_value, 1, max_value);
}

bool Client::get_webhook_fix_ip_address(const Query *query) {
  if (query->is_internal()) {
    return query->has_arg("fix_ip_address");
  }
  return !query->arg("ip_address").empty();
}

void Client::do_set_webhook(PromisedQueryPtr query, bool was_deleted) {
  CHECK(webhook_url_.empty());
  if (logging_out_ || closing_) {
    return fail_query_closing(std::move(query));
  }
  if (to_bool(query->arg("drop_pending_updates"))) {
    clear_tqueue();
  }
  td::Slice new_url;
  if (query->method() == "setwebhook") {
    new_url = query->arg("url");
  }
  if (!new_url.empty()) {
    auto url = td::parse_url(new_url, td::HttpUrl::Protocol::Https);
    if (url.is_error()) {
      return fail_query(400, "Bad Request: invalid webhook URL specified", std::move(query));
    }
    auto secret_token = query->arg("secret_token");
    if (secret_token.size() > 256) {
      return fail_query(400, "Bad Request: secret token is too long", std::move(query));
    }
    if (!td::is_base64url_characters(secret_token)) {
      return fail_query(400, "Bad Request: secret token contains illegal characters", std::move(query));
    }

    if (active_webhook_set_query_) {
      // shouldn't happen, unless the active setWebhook request took more than 1 second
      return query->set_retry_after_error(1);
    }

    CHECK(!has_webhook_certificate_);
    if (query->is_internal()) {
      has_webhook_certificate_ = query->arg("certificate") == "previous";
    } else {
      auto *cert_file_ptr = get_webhook_certificate(query.get());
      if (cert_file_ptr != nullptr) {
        auto size = cert_file_ptr->size;
        if (size > MAX_CERTIFICATE_FILE_SIZE) {
          return fail_query(400, PSLICE() << "Bad Request: certificate size is too big (" << size << " bytes)",
                            std::move(query));
        }
        CHECK(!webhook_set_query_);
        active_webhook_set_query_ = std::move(query);
        td::Scheduler::instance()->run_on_scheduler(
            SharedData::get_webhook_certificate_scheduler_id(),
            [actor_id = actor_id(this), from_path = cert_file_ptr->temp_file_name,
             to_path = get_webhook_certificate_path(), size](td::Unit) {
              LOG(INFO) << "Copy certificate to " << to_path;
              auto status = td::copy_file(from_path, to_path, size);
              send_closure(actor_id, &Client::on_webhook_certificate_copied, std::move(status));
            });
        return;
      }
    }
    finish_set_webhook(std::move(query));
  } else {
    answer_query(td::JsonTrue(), std::move(query),
                 was_deleted ? td::Slice("Webhook was deleted") : td::Slice("Webhook is already deleted"));
  }
}

void Client::on_webhook_certificate_copied(td::Status status) {
  CHECK(active_webhook_set_query_);
  if (status.is_error()) {
    return fail_query(500, "Internal Server Error: failed to save certificate", std::move(active_webhook_set_query_));
  }
  has_webhook_certificate_ = true;
  finish_set_webhook(std::move(active_webhook_set_query_));
}

void Client::finish_set_webhook(PromisedQueryPtr query) {
  CHECK(!active_webhook_set_query_);
  CHECK(!webhook_set_query_);
  CHECK(webhook_url_.empty());
  if (logging_out_ || closing_) {
    return fail_query_closing(std::move(query));
  }
  td::Slice new_url = query->arg("url");
  CHECK(!new_url.empty());
  webhook_url_ = new_url.str();
  webhook_set_time_ = td::Time::now();
  webhook_max_connections_ = get_webhook_max_connections(query.get());
  webhook_secret_token_ = query->arg("secret_token").str();
  webhook_ip_address_ = query->arg("ip_address").str();
  webhook_fix_ip_address_ = get_webhook_fix_ip_address(query.get());
  last_webhook_error_date_ = 0;
  last_webhook_error_ = td::Status::OK();

  update_allowed_update_types(query.get());

  auto url = td::parse_url(new_url, td::HttpUrl::Protocol::Https);
  CHECK(url.is_ok());

  LOG(WARNING) << "Create " << (has_webhook_certificate_ ? "self-signed " : "") << "webhook: " << new_url;
  auto webhook_actor_name = PSTRING() << "Webhook " << url.ok();
  webhook_id_ = td::create_actor<WebhookActor>(
      webhook_actor_name, actor_shared(this, webhook_generation_), tqueue_id_, url.move_as_ok(),
      has_webhook_certificate_ ? get_webhook_certificate_path() : td::string(), webhook_max_connections_,
      query->is_internal(), webhook_ip_address_, webhook_fix_ip_address_, webhook_secret_token_, parameters_);
  // wait for webhook verified or webhook callback
  webhook_query_type_ = WebhookQueryType::Verify;
  CHECK(!active_webhook_set_query_);
  webhook_set_query_ = std::move(query);
}

void Client::delete_last_send_message_time(td::int64 file_size, double max_delay) {
  auto last_send_message_time = last_send_message_time_.get(file_size);
  if (last_send_message_time == 0.0) {
    return;
  }
  if (last_send_message_time < td::Time::now() - max_delay) {
    LOG(DEBUG) << "Clear last send message time for size " << file_size;
    last_send_message_time_.erase(file_size);
  }
}

void Client::do_send_message(object_ptr<td_api::InputMessageContent> input_message_content, PromisedQueryPtr query,
                             bool force) {
  auto chat_id = query->arg("chat_id");
  auto forum_topic_id = get_forum_topic_id(query.get(), "message_thread_id");
  auto business_connection_id = query->arg("business_connection_id");
  auto r_ephemeral_message_parameters = get_ephemeral_message_parameters(query.get());
  if (r_ephemeral_message_parameters.is_error()) {
    return fail_query_with_error(std::move(query), 400, r_ephemeral_message_parameters.error().message());
  }
  auto r_reply_parameters = get_reply_parameters(query.get());
  if (r_reply_parameters.is_error()) {
    return fail_query_with_error(std::move(query), 400, r_reply_parameters.error().message());
  }
  auto reply_parameters = r_reply_parameters.move_as_ok();
  auto disable_notification = to_bool(query->arg("disable_notification"));
  auto protect_content = to_bool(query->arg("protect_content"));
  auto allow_paid_broadcast = to_bool(query->arg("allow_paid_broadcast"));
  auto effect_id = td::to_integer<int64>(query->arg("message_effect_id"));
  auto direct_messages_topic_id = td::to_integer<int64>(query->arg("direct_messages_topic_id"));
  auto r_input_suggested_post_info = get_input_suggested_post_info(query.get());
  if (r_input_suggested_post_info.is_error()) {
    return fail_query_with_error(std::move(query), 400, r_input_suggested_post_info.error().message());
  }
  auto r_reply_markup = get_reply_markup(query.get(), bot_user_ids_);
  if (r_reply_markup.is_error()) {
    return fail_query_with_error(std::move(query), 400, r_reply_markup.error().message());
  }
  auto reply_markup = r_reply_markup.move_as_ok();

  auto send_options = get_message_send_options(disable_notification, protect_content, allow_paid_broadcast, effect_id,
                                               r_input_suggested_post_info.move_as_ok());
  resolve_reply_markup_bot_usernames(
      std::move(reply_markup), std::move(query),
      [this, chat_id_str = chat_id.str(), forum_topic_id, direct_messages_topic_id,
       business_connection_id = business_connection_id.str(),
       ephemeral_message_parameters = r_ephemeral_message_parameters.move_as_ok(),
       reply_parameters = std::move(reply_parameters), disable_notification, protect_content, allow_paid_broadcast,
       effect_id, send_options = std::move(send_options), input_message_content = std::move(input_message_content)](
          object_ptr<td_api::ReplyMarkup> reply_markup, PromisedQueryPtr query) mutable {
        if (!business_connection_id.empty()) {
          return check_business_connection_chat_id(
              business_connection_id, chat_id_str, std::move(query),
              [this, reply_parameters = std::move(reply_parameters), disable_notification, protect_content, effect_id,
               reply_markup = std::move(reply_markup), input_message_content = std::move(input_message_content)](
                  const BusinessConnection *business_connection, int64 chat_id, PromisedQueryPtr query) mutable {
                send_request(
                    make_object<td_api::sendBusinessMessage>(
                        business_connection->id_, chat_id,
                        get_input_message_reply_to_unchecked(std::move(reply_parameters)), disable_notification,
                        protect_content, effect_id, std::move(reply_markup), std::move(input_message_content)),
                    td::make_unique<TdOnReturnBusinessMessageCallback>(this, business_connection->id_,
                                                                       std::move(query)));
              });
        }

        auto on_success = [this, ephemeral_message_parameters, send_options = std::move(send_options),
                           input_message_content = std::move(input_message_content),
                           reply_markup = std::move(reply_markup)](
                              int64 chat_id, object_ptr<td_api::MessageTopic> topic_id,
                              CheckedReplyParameters reply_parameters, PromisedQueryPtr query) mutable {
          auto &count = yet_unsent_message_count_[chat_id];
          if (count >= MAX_CONCURRENTLY_SENT_CHAT_MESSAGES) {
            return fail_query_flood_limit_exceeded(std::move(query));
          }
          count++;

          if (ephemeral_message_parameters.receiver_user_id != 0) {
            return send_request(
                make_object<td_api::sendEphemeralMessage>(
                    chat_id, std::move(topic_id), ephemeral_message_parameters.receiver_user_id,
                    ephemeral_message_parameters.callback_query_id,
                    ephemeral_message_parameters.replace_callback_query_message,
                    get_input_message_reply_to(std::move(reply_parameters)), send_options->protect_content_, 0, false,
                    std::move(reply_markup), std::move(input_message_content)),
                td::make_unique<TdOnSendMessageCallback>(this, chat_id, std::move(query)));
          }

          send_request(make_object<td_api::sendMessage>(
                           chat_id, std::move(topic_id), get_input_message_reply_to(std::move(reply_parameters)),
                           std::move(send_options), std::move(reply_markup), std::move(input_message_content)),
                       td::make_unique<TdOnSendMessageCallback>(this, chat_id, std::move(query)));
        };
        check_reply_parameters(chat_id_str, std::move(reply_parameters), forum_topic_id, direct_messages_topic_id,
                               std::move(query), std::move(on_success), allow_paid_broadcast);
      });
}

td::int64 Client::get_send_message_query_id(PromisedQueryPtr query, bool is_multisend) {
  auto query_id = current_send_message_query_id_++;
  auto &pending_query = pending_send_message_queries_[query_id];
  CHECK(pending_query == nullptr);
  pending_query = td::make_unique<PendingSendMessageQuery>();
  pending_query->query = std::move(query);
  pending_query->is_multisend = is_multisend;
  return query_id;
}

void Client::on_sent_message(object_ptr<td_api::message> &&message, int64 query_id) {
  CHECK(message != nullptr);
  int64 chat_id = message->chat_id_;
  int64 message_id = message->id_;

  MessageFullId yet_unsent_message_full_id{chat_id, message_id};
  YetUnsentMessage yet_unsent_message;
  yet_unsent_message.send_message_query_id = query_id;
  auto emplace_result = yet_unsent_messages_.emplace(yet_unsent_message_full_id, yet_unsent_message);
  CHECK(emplace_result.second);

  auto &query = *pending_send_message_queries_[query_id];
  query.awaited_message_count++;
  query.total_message_count++;
}

void Client::on_sent_story(object_ptr<td_api::story> &&story, PromisedQueryPtr query) {
  CHECK(story != nullptr);
  int64 chat_id = story->poster_chat_id_;
  int64 story_id = story->id_;

  MessageFullId story_full_id{chat_id, story_id};
  YetUnsentStory yet_unsent_story;
  yet_unsent_story.query = std::move(query);
  auto emplace_result = yet_unsent_stories_.emplace(story_full_id, std::move(yet_unsent_story));
  CHECK(emplace_result.second);
}

td::Status Client::do_edit_ephemeral_message(object_ptr<td_api::InputMessageContent> input_message_content,
                                             PromisedQueryPtr &query) {
  auto chat_id = query->arg("chat_id");
  TRY_RESULT(receiver_user_id, get_user_id(query.get(), "receiver_user_id"));
  auto ephemeral_message_id = get_integer_arg(query.get(), "ephemeral_message_id", 0);
  TRY_RESULT(reply_markup, get_reply_markup(query.get(), bot_user_ids_));

  resolve_reply_markup_bot_usernames(
      std::move(reply_markup), std::move(query),
      [this, chat_id_str = chat_id.str(), receiver_user_id, ephemeral_message_id,
       input_message_content = std::move(input_message_content)](object_ptr<td_api::ReplyMarkup> reply_markup,
                                                                 PromisedQueryPtr query) mutable {
        check_chat(
            chat_id_str, AccessRights::Edit, std::move(query),
            [this, receiver_user_id, ephemeral_message_id, input_message_content = std::move(input_message_content),
             reply_markup = std::move(reply_markup)](int64 chat_id, PromisedQueryPtr query) mutable {
              send_request(
                  make_object<td_api::editEphemeralMessage>(chat_id, receiver_user_id, ephemeral_message_id,
                                                            std::move(reply_markup), std::move(input_message_content)),
                  td::make_unique<TdOnOkQueryCallback>(std::move(query)));
            });
      });
  return td::Status::OK();
}

void Client::abort_long_poll(bool from_set_webhook) {
  if (long_poll_query_) {
    td::CSlice message;
    if (from_set_webhook) {
      message = "Conflict: terminated by setWebhook request";
    } else {
      message = "Conflict: terminated by other getUpdates request; make sure that only one bot instance is running";
    }
    fail_query_conflict(message, std::move(long_poll_query_));
  }
}

void Client::fail_query_conflict(td::CSlice message, PromisedQueryPtr &&query) {
  auto now = td::Time::now_cached();
  if (now >= next_get_updates_conflict_time_) {
    fail_query(409, message, std::move(query));
    next_get_updates_conflict_time_ = now + 3.0;
  } else {
    td::create_actor<td::SleepActor>(
        "FailQueryConflictSleepActor", 3.0,
        td::PromiseCreator::lambda([message = message.str(), query = std::move(query)](td::Result<> result) mutable {
          fail_query(409, message, std::move(query));
        }))
        .release();
  }
}

void Client::fail_query_closing(PromisedQueryPtr &&query) {
  auto error = get_closing_error();
  if (error.retry_after > 0) {
    query->set_retry_after_error(error.retry_after);
  } else {
    fail_query(error.code, error.message, std::move(query));
  }
}

void Client::fail_query_flood_limit_exceeded(PromisedQueryPtr &&query) {
  flood_limited_query_count_++;
  td::create_actor<td::SleepActor>(
      "FailQueryFloodLimitExceededActor", 3.0,
      td::PromiseCreator::lambda(
          [query = std::move(query)](td::Result<td::Unit> result) mutable { query->set_retry_after_error(60); }))
      .release();
}

Client::ClosingError Client::get_closing_error() {
  ClosingError result;
  result.retry_after = 0;
  if (logging_out_) {
    if (is_api_id_invalid_) {
      result.code = 401;
      result.message = "Unauthorized: invalid api-id/api-hash";
    } else if (next_authorization_time_ > 0.0) {
      result.code = 429;
      result.retry_after = td::max(static_cast<int>(next_authorization_time_ - td::Time::now()), 0) + 1;
      if (result.retry_after != prev_retry_after) {
        prev_retry_after = result.retry_after;
        retry_after_error_message = PSTRING() << "Too Many Requests: retry after " << result.retry_after;
      }
      result.message = retry_after_error_message;
    } else if (clear_tqueue_) {
      result.code = 400;
      result.message = "Logged out";
    } else {
      result.code = 401;
      result.message = "Unauthorized";
    }
  } else {
    CHECK(closing_);
    result.code = 500;
    result.message = "Internal Server Error: restart";
  }
  return result;
}

class Client::JsonUpdates final : public td::Jsonable {
 public:
  explicit JsonUpdates(td::Span<td::TQueue::Event> updates) : updates_(updates) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    int left_len = 1 << 22;
    for (auto &update : updates_) {
      left_len -= 50 + td::narrow_cast<int>(update.data.size());
      if (left_len <= 0) {
        break;
      }
      array << JsonUpdate(update.id.value(), update.data);
    }
  }

 private:
  td::Span<td::TQueue::Event> updates_;
};

void Client::do_get_updates(int32 offset, int32 limit, int32 timeout, PromisedQueryPtr query) {
  auto &tqueue = parameters_->shared_data_->tqueue_;
  LOG(DEBUG) << "Get updates with offset = " << offset << ", limit = " << limit << " and timeout = " << timeout;
  LOG(DEBUG) << "Queue head = " << tqueue->get_head(tqueue_id_) << ", queue tail = " << tqueue->get_tail(tqueue_id_);

  if (offset < 0) {
    auto deleted_events = tqueue->clear(tqueue_id_, -offset);
    td::Scheduler::instance()->destroy_on_scheduler(SharedData::get_file_gc_scheduler_id(), deleted_events);
  }
  if (offset <= 0) {
    offset = tqueue->get_head(tqueue_id_).value();
  }

  td::MutableSpan<td::TQueue::Event> updates(parameters_->shared_data_->event_buffer_,
                                             SharedData::TQUEUE_EVENT_BUFFER_SIZE);
  updates.truncate(limit);
  td::TQueue::EventId from;
  size_t total_size = 0;
  if (offset <= 0) {
    // queue is not created yet
    updates = {};
  } else {
    bool is_ok = false;
    auto r_offset = td::TQueue::EventId::from_int32(offset);
    auto now = get_unix_time();
    if (r_offset.is_ok()) {
      from = r_offset.ok();
      auto r_total_size = tqueue->get(tqueue_id_, from, true, now, updates);
      if (r_total_size.is_ok()) {
        is_ok = true;
        total_size = r_total_size.move_as_ok();
      }
    }
    if (!is_ok) {
      from = tqueue->get_head(tqueue_id_);
      auto r_total_size = tqueue->get(tqueue_id_, from, true, now, updates);
      CHECK(r_total_size.is_ok());
      total_size = r_total_size.move_as_ok();
    }
  }
  CHECK(total_size >= updates.size());
  total_size -= updates.size();

  bool need_warning = total_size > 0 && (query->start_timestamp() - previous_get_updates_finish_time_ > 5.0);
  if (total_size <= MIN_PENDING_UPDATES_WARNING / 2) {
    if (last_pending_update_count_ > MIN_PENDING_UPDATES_WARNING) {
      need_warning = true;
      last_pending_update_count_ = MIN_PENDING_UPDATES_WARNING;
    }
  } else if (total_size >= last_pending_update_count_) {
    need_warning = true;
    while (total_size >= last_pending_update_count_) {
      last_pending_update_count_ *= 2;
    }
  }
  if (need_warning && previous_get_updates_finish_time_ > 0 &&
      query->start_timestamp() > previous_get_updates_finish_time_) {
    LOG(WARNING) << "Found " << updates.size() << " updates out of " << (total_size + updates.size())
                 << " after last getUpdates call " << (query->start_timestamp() - previous_get_updates_finish_time_)
                 << " seconds ago in " << (td::Time::now() - query->start_timestamp()) << " seconds from "
                 << query->get_peer_ip_address();
  } else {
    LOG(DEBUG) << "Found " << updates.size() << " updates out of " << total_size << " from " << from;
  }

  if (timeout != 0 && updates.size() == 0) {
    abort_long_poll(false);
    long_poll_offset_ = offset;
    long_poll_limit_ = limit;
    long_poll_query_ = std::move(query);
    long_poll_was_wakeup_ = false;
    long_poll_hard_timeout_ = td::Time::now_cached() + timeout;
    long_poll_slot_.set_event(td::EventCreator::raw(actor_id(), static_cast<td::uint64>(0)));
    long_poll_slot_.set_timeout_at(long_poll_hard_timeout_);
    return;
  }
  previous_get_updates_finish_time_ = td::Time::now();
  next_bot_updates_warning_time_ = td::Time::now() + BOT_UPDATES_WARNING_DELAY;
  if (total_size == updates.size() && was_bot_updates_warning_) {
    send_request(make_object<td_api::setBotUpdatesStatus>(0, ""), td::make_unique<TdOnOkCallback>());
    was_bot_updates_warning_ = false;
  }
  answer_query(JsonUpdates(updates), std::move(query));
}

void Client::long_poll_wakeup(bool force_flag) {
  if (!long_poll_query_) {
    auto pending_update_count = get_pending_update_count();
    if (pending_update_count >= MIN_PENDING_UPDATES_WARNING && td::Time::now() > next_bot_updates_warning_time_) {
      send_request(make_object<td_api::setBotUpdatesStatus>(td::narrow_cast<int32>(pending_update_count),
                                                            "The getUpdates method is not called for too long"),
                   td::make_unique<TdOnOkCallback>());
      next_bot_updates_warning_time_ =
          td::Time::now_cached() + BOT_UPDATES_WARNING_DELAY;  // do not send warnings too often
      was_bot_updates_warning_ = true;
    }
    return;
  }
  if (force_flag) {
    do_get_updates(long_poll_offset_, long_poll_limit_, 0, std::move(long_poll_query_));
  } else {
    double now = td::Time::now();
    if (!long_poll_was_wakeup_) {
      long_poll_hard_timeout_ = td::min(now + LONG_POLL_MAX_DELAY, long_poll_hard_timeout_);
      long_poll_was_wakeup_ = true;
    }
    double timeout = td::min(now + LONG_POLL_WAIT_AFTER, long_poll_hard_timeout_);
    long_poll_slot_.set_event(td::EventCreator::raw(actor_id(), static_cast<td::uint64>(0)));
    long_poll_slot_.set_timeout_at(timeout);
  }
}

void Client::add_user(UserInfo *user_info, object_ptr<td_api::user> &&user) {
  user_info->first_name = std::move(user->first_name_);
  user_info->last_name = std::move(user->last_name_);
  if (user->usernames_ == nullptr) {
    user_info->active_usernames.clear();
    user_info->editable_username.clear();
  } else {
    user_info->active_usernames = std::move(user->usernames_->active_usernames_);
    user_info->editable_username = std::move(user->usernames_->editable_username_);
  }
  user_info->language_code = std::move(user->language_code_);
  user_info->paid_message_star_count = user->paid_message_star_count_;
  user_info->have_access = user->have_access_;
  user_info->is_premium = user->is_premium_;
  user_info->added_to_attachment_menu = user->added_to_attachment_menu_;

  switch (user->type_->get_id()) {
    case td_api::userTypeRegular::ID:
      user_info->type = UserInfo::Type::Regular;
      break;
    case td_api::userTypeBot::ID: {
      user_info->type = UserInfo::Type::Bot;
      auto *bot = static_cast<const td_api::userTypeBot *>(user->type_.get());
      user_info->can_join_groups = bot->can_join_groups_;
      user_info->can_read_all_group_messages = bot->can_read_all_group_messages_;
      user_info->is_inline_bot = bot->is_inline_;
      user_info->supports_guest_queries = bot->supports_guest_queries_;
      user_info->can_connect_to_business = bot->can_connect_to_business_;
      user_info->has_main_web_app = bot->has_main_web_app_;
      user_info->has_topics = bot->has_topics_;
      user_info->allows_users_to_create_topics = bot->allows_users_to_create_topics_;
      user_info->can_manage_bots = bot->can_manage_bots_;
      user_info->is_guard = bot->is_guard_;
      break;
    }
    case td_api::userTypeDeleted::ID:
      user_info->type = UserInfo::Type::Deleted;
      break;
    case td_api::userTypeUnknown::ID:
      user_info->type = UserInfo::Type::Unknown;
      break;
    default:
      UNREACHABLE();
      break;
  }
}

Client::UserInfo *Client::add_user_info(int64 user_id) {
  auto &user_info = users_[user_id];
  if (user_info == nullptr) {
    user_info = td::make_unique<UserInfo>();
  }
  return user_info.get();
}

const Client::UserInfo *Client::get_user_info(int64 user_id) const {
  return users_.get_pointer(user_id);
}

void Client::add_group(GroupInfo *group_info, object_ptr<td_api::basicGroup> &&group) {
  group_info->member_count = group->member_count_;
  group_info->left = group->status_->get_id() == td_api::chatMemberStatusLeft::ID;
  group_info->kicked = group->status_->get_id() == td_api::chatMemberStatusBanned::ID;
  group_info->is_active = group->is_active_;
  group_info->upgraded_to_supergroup_id = group->upgraded_to_supergroup_id_;
  if (!group_info->left && !group_info->kicked && group_info->member_count == 0) {
    group_info->member_count = 1;
  }
}

Client::GroupInfo *Client::add_group_info(int64 group_id) {
  auto &group_info = groups_[group_id];
  if (group_info == nullptr) {
    group_info = td::make_unique<GroupInfo>();
  }
  return group_info.get();
}

const Client::GroupInfo *Client::get_group_info(int64 group_id) const {
  return groups_.get_pointer(group_id);
}

void Client::add_supergroup(SupergroupInfo *supergroup_info, object_ptr<td_api::supergroup> &&supergroup) {
  if (supergroup->usernames_ == nullptr) {
    supergroup_info->active_usernames.clear();
    supergroup_info->editable_username.clear();
  } else {
    supergroup_info->active_usernames = std::move(supergroup->usernames_->active_usernames_);
    supergroup_info->editable_username = std::move(supergroup->usernames_->editable_username_);
  }
  supergroup_info->date = supergroup->date_;
  supergroup_info->status = std::move(supergroup->status_);
  supergroup_info->paid_message_star_count = supergroup->paid_message_star_count_;
  supergroup_info->is_supergroup = !supergroup->is_channel_;
  supergroup_info->is_forum = supergroup->is_forum_;
  supergroup_info->is_direct_messages = supergroup->is_direct_messages_group_;
  supergroup_info->has_location = supergroup->has_location_;
  supergroup_info->join_to_send_messages = supergroup->join_to_send_messages_;
  supergroup_info->join_by_request = supergroup->join_by_request_;
}

Client::SupergroupInfo *Client::add_supergroup_info(int64 supergroup_id) {
  auto &supergroup_info = supergroups_[supergroup_id];
  if (supergroup_info == nullptr) {
    supergroup_info = td::make_unique<SupergroupInfo>();
  }
  return supergroup_info.get();
}

const Client::SupergroupInfo *Client::get_supergroup_info(int64 supergroup_id) const {
  return supergroups_.get_pointer(supergroup_id);
}

Client::ChatInfo *Client::add_chat(int64 chat_id) {
  auto &chat_info = chats_[chat_id];
  if (chat_info == nullptr) {
    chat_info = td::make_unique<ChatInfo>();
  }
  return chat_info.get();
}

const Client::ChatInfo *Client::get_chat(int64 chat_id) const {
  return chats_.get_pointer(chat_id);
}

void Client::add_community(CommunityInfo *community_info, object_ptr<td_api::community> &&community) {
  community_info->name = std::move(community->name_);
}

Client::CommunityInfo *Client::add_community_info(int64 community_id) {
  auto &community_info = communities_[community_id];
  if (community_info == nullptr) {
    community_info = td::make_unique<CommunityInfo>();
  }
  return community_info.get();
}

const Client::CommunityInfo *Client::get_community_info(int64 community_id) const {
  return communities_.get_pointer(community_id);
}

void Client::set_chat_available_reactions(ChatInfo *chat_info,
                                          object_ptr<td_api::ChatAvailableReactions> &&available_reactions) {
  CHECK(chat_info != nullptr);
  CHECK(available_reactions != nullptr);
  switch (available_reactions->get_id()) {
    case td_api::chatAvailableReactionsSome::ID:
      chat_info->available_reactions = move_object_as<td_api::chatAvailableReactionsSome>(available_reactions);
      chat_info->max_reaction_count = chat_info->available_reactions->max_reaction_count_;
      break;
    case td_api::chatAvailableReactionsAll::ID:
      chat_info->available_reactions = nullptr;
      chat_info->max_reaction_count =
          static_cast<const td_api::chatAvailableReactionsAll *>(available_reactions.get())->max_reaction_count_;
      break;
    default:
      UNREACHABLE();
  }
}

Client::ChatType Client::get_chat_type(int64 chat_id) const {
  auto chat_info = get_chat(chat_id);
  if (chat_info == nullptr) {
    return ChatType::Unknown;
  }
  switch (chat_info->type) {
    case ChatInfo::Type::Private:
      return ChatType::Private;
    case ChatInfo::Type::Group:
      return ChatType::Group;
    case ChatInfo::Type::Supergroup: {
      auto supergroup_info = get_supergroup_info(chat_info->supergroup_id);
      if (supergroup_info == nullptr) {
        return ChatType::Unknown;
      }
      if (supergroup_info->is_supergroup) {
        return ChatType::Supergroup;
      } else {
        return ChatType::Channel;
      }
    }
    case ChatInfo::Type::Unknown:
      return ChatType::Unknown;
    default:
      UNREACHABLE();
      return ChatType::Unknown;
  }
}

td::string Client::get_chat_description(int64 chat_id) const {
  auto chat_info = get_chat(chat_id);
  if (chat_info == nullptr) {
    return PSTRING() << "unknown chat " << chat_id;
  }
  switch (chat_info->type) {
    case ChatInfo::Type::Private: {
      auto user_info = get_user_info(chat_info->user_id);
      return PSTRING() << "private " << (user_info == nullptr || !user_info->have_access ? "un" : "")
                       << "accessible chat " << chat_id;
    }
    case ChatInfo::Type::Group: {
      auto group_info = get_group_info(chat_info->group_id);
      if (group_info == nullptr) {
        return PSTRING() << "unknown group chat " << chat_id;
      }
      return PSTRING() << (group_info->is_active ? "" : "in") << "active group chat " << chat_id << ", chat status = "
                       << (group_info->kicked ? "kicked" : (group_info->left ? "left" : "member"));
    }
    case ChatInfo::Type::Supergroup: {
      auto supergroup_info = get_supergroup_info(chat_info->supergroup_id);
      if (supergroup_info == nullptr) {
        return PSTRING() << "unknown supergroup chat " << chat_id;
      }
      return PSTRING() << (supergroup_info->is_supergroup ? "supergroup" : "channel") << " chat " << chat_id
                       << ", chat status = " << to_string(supergroup_info->status)
                       << ", usernames = " << supergroup_info->active_usernames;
    }
    case ChatInfo::Type::Unknown:
      return PSTRING() << "unknown chat " << chat_id;
    default:
      UNREACHABLE();
      return "";
  }
}

const Client::BusinessConnection *Client::add_business_connection(
    object_ptr<td_api::businessConnection> &&business_connection, bool from_update) {
  CHECK(business_connection != nullptr);
  auto &connection = business_connections_[business_connection->id_];
  if (connection == nullptr) {
    connection = td::make_unique<BusinessConnection>();
  } else if (!from_update) {
    return connection.get();
  }
  connection->id_ = std::move(business_connection->id_);
  connection->user_id_ = business_connection->user_id_;
  connection->user_chat_id_ = business_connection->user_chat_id_;
  connection->date_ = business_connection->date_;
  connection->rights_ = std::move(business_connection->rights_);
  connection->is_enabled_ = business_connection->is_enabled_;
  return connection.get();
}

const Client::BusinessConnection *Client::get_business_connection(const td::string &connection_id) const {
  return business_connections_.get_pointer(connection_id);
}

void Client::json_store_file(td::JsonObjectScope &object, const td_api::file *file, bool with_path) const {
  if (file->id_ == 0) {
    return;
  }

  LOG_IF(ERROR, file->remote_->id_.empty()) << "File remote identifier is empty: " << td::oneline(to_string(*file));

  object("file_id", file->remote_->id_);
  object("file_unique_id", file->remote_->unique_id_);
  if (file->size_) {
    object("file_size", file->size_);
  }
  if (with_path && file->local_->is_downloading_completed_) {
    if (parameters_->local_mode_) {
      if (td::check_utf8(file->local_->path_)) {
        object("file_path", file->local_->path_);
      } else {
        object("file_path", td::JsonRawString(file->local_->path_));
      }
    } else {
      td::Slice relative_path = td::PathView::relative(file->local_->path_, files_dir_, true);
      if (!relative_path.empty() && file->local_->downloaded_size_ <= MAX_DOWNLOAD_FILE_SIZE) {
        object("file_path", relative_path);
      }
    }
  }
}

void Client::json_store_thumbnail(td::JsonObjectScope &object, const td_api::thumbnail *thumbnail) const {
  if (thumbnail == nullptr || thumbnail->format_->get_id() == td_api::thumbnailFormatMpeg4::ID) {
    return;
  }

  CHECK(thumbnail->file_->id_ > 0);
  object("thumbnail", JsonThumbnail(thumbnail, this));
  object("thumb", JsonThumbnail(thumbnail, this));
}

void Client::json_store_callback_query_payload(td::JsonObjectScope &object,
                                               const td_api::CallbackQueryPayload *payload) {
  CHECK(payload != nullptr);
  switch (payload->get_id()) {
    case td_api::callbackQueryPayloadData::ID: {
      auto data = static_cast<const td_api::callbackQueryPayloadData *>(payload);
      if (!td::check_utf8(data->data_)) {
        LOG(WARNING) << "Receive non-UTF-8 callback query data";
        object("data", td::JsonRawString(data->data_));
      } else {
        object("data", data->data_);
      }
      break;
    }
    case td_api::callbackQueryPayloadGame::ID:
      object("game_short_name", static_cast<const td_api::callbackQueryPayloadGame *>(payload)->game_short_name_);
      break;
    case td_api::callbackQueryPayloadDataWithPassword::ID:
      UNREACHABLE();
      break;
    default:
      UNREACHABLE();
  }
}

void Client::json_store_administrator_rights(td::JsonObjectScope &object, const td_api::chatAdministratorRights *rights,
                                             ChatType chat_type) {
  object("can_manage_chat", td::JsonBool(rights->can_manage_chat_));
  object("can_change_info", td::JsonBool(rights->can_change_info_));
  if (chat_type == ChatType::Channel) {
    object("can_post_messages", td::JsonBool(rights->can_post_messages_));
    object("can_edit_messages", td::JsonBool(rights->can_edit_messages_));
  }
  object("can_delete_messages", td::JsonBool(rights->can_delete_messages_));
  object("can_invite_users", td::JsonBool(rights->can_invite_users_));
  object("can_restrict_members", td::JsonBool(rights->can_restrict_members_));
  if (chat_type == ChatType::Group || chat_type == ChatType::Supergroup) {
    object("can_pin_messages", td::JsonBool(rights->can_pin_messages_));
  }
  if (chat_type == ChatType::Supergroup) {
    object("can_manage_topics", td::JsonBool(rights->can_manage_topics_));
  }
  object("can_promote_members", td::JsonBool(rights->can_promote_members_));
  object("can_manage_video_chats", td::JsonBool(rights->can_manage_video_chats_));
  object("can_post_stories", td::JsonBool(rights->can_post_stories_));
  object("can_edit_stories", td::JsonBool(rights->can_edit_stories_));
  object("can_delete_stories", td::JsonBool(rights->can_delete_stories_));
  if (chat_type == ChatType::Channel) {
    object("can_manage_direct_messages", td::JsonBool(rights->can_manage_direct_messages_));
  }
  if (chat_type == ChatType::Group || chat_type == ChatType::Supergroup) {
    object("can_manage_tags", td::JsonBool(rights->can_manage_tags_));
  }
  object("can_send_welcome_messages", td::JsonBool(rights->can_send_welcome_messages_));
  object("is_anonymous", td::JsonBool(rights->is_anonymous_));
}

void Client::json_store_permissions(td::JsonObjectScope &object, const td_api::chatPermissions *permissions) {
  bool can_send_media_messages = permissions->can_send_audios_ || permissions->can_send_documents_ ||
                                 permissions->can_send_photos_ || permissions->can_send_videos_ ||
                                 permissions->can_send_video_notes_ || permissions->can_send_voice_notes_;
  object("can_send_messages", td::JsonBool(permissions->can_send_basic_messages_));
  object("can_send_media_messages", td::JsonBool(can_send_media_messages));
  object("can_send_audios", td::JsonBool(permissions->can_send_audios_));
  object("can_send_documents", td::JsonBool(permissions->can_send_documents_));
  object("can_send_photos", td::JsonBool(permissions->can_send_photos_));
  object("can_send_videos", td::JsonBool(permissions->can_send_videos_));
  object("can_send_video_notes", td::JsonBool(permissions->can_send_video_notes_));
  object("can_send_voice_notes", td::JsonBool(permissions->can_send_voice_notes_));
  object("can_send_polls", td::JsonBool(permissions->can_send_polls_));
  object("can_send_other_messages", td::JsonBool(permissions->can_send_other_messages_));
  object("can_add_web_page_previews", td::JsonBool(permissions->can_add_link_previews_));
  object("can_react_to_messages", td::JsonBool(permissions->can_react_to_messages_));
  object("can_edit_tag", td::JsonBool(permissions->can_edit_tag_));
  object("can_change_info", td::JsonBool(permissions->can_change_info_));
  object("can_invite_users", td::JsonBool(permissions->can_invite_users_));
  object("can_pin_messages", td::JsonBool(permissions->can_pin_messages_));
  object("can_manage_topics", td::JsonBool(permissions->can_create_topics_));
}

void Client::json_store_rarity(td::JsonObjectScope &object, const td_api::UpgradedGiftAttributeRarity *rarity) {
  int32 rarity_per_mille = 0;
  switch (rarity->get_id()) {
    case td_api::upgradedGiftAttributeRarityPerMille::ID:
      rarity_per_mille = static_cast<const td_api::upgradedGiftAttributeRarityPerMille *>(rarity)->per_mille_;
      break;
    case td_api::upgradedGiftAttributeRarityUncommon::ID:
      object("rarity", "uncommon");
      break;
    case td_api::upgradedGiftAttributeRarityRare::ID:
      object("rarity", "rare");
      break;
    case td_api::upgradedGiftAttributeRarityEpic::ID:
      object("rarity", "epic");
      break;
    case td_api::upgradedGiftAttributeRarityLegendary::ID:
      object("rarity", "legendary");
      break;
    default:
      UNREACHABLE();
  }
  object("rarity_per_mille", rarity_per_mille);
}

void Client::json_store_style(td::JsonObjectScope &object, const td_api::ButtonStyle *style, bool for_rich_message) {
  CHECK(style != nullptr);
  switch (style->get_id()) {
    case td_api::buttonStyleDefault::ID:
      break;
    case td_api::buttonStylePrimary::ID:
      object("style", "primary");
      break;
    case td_api::buttonStyleDanger::ID:
      object("style", "danger");
      break;
    case td_api::buttonStyleSuccess::ID:
      object("style", "success");
      break;
    case td_api::buttonStyleLink::ID:
      if (for_rich_message) {
        object("style", "link");
      } else {
        LOG(ERROR) << "Receive buttonStyleLink";
      }
      break;
    default:
      UNREACHABLE();
  }
}

void Client::json_store_inline_keyboard_button_type(td::JsonObjectScope &object,
                                                    const td_api::InlineKeyboardButtonType *button_type,
                                                    bool for_rich_message) {
  CHECK(button_type != nullptr);
  switch (button_type->get_id()) {
    case td_api::inlineKeyboardButtonTypeUrl::ID: {
      auto type = static_cast<const td_api::inlineKeyboardButtonTypeUrl *>(button_type);
      object("url", type->url_);
      break;
    }
    case td_api::inlineKeyboardButtonTypeLoginUrl::ID: {
      auto type = static_cast<const td_api::inlineKeyboardButtonTypeLoginUrl *>(button_type);
      object("url", type->url_);
      break;
    }
    case td_api::inlineKeyboardButtonTypeCallback::ID:
    case td_api::inlineKeyboardButtonTypeCallbackWithPassword::ID: {
      auto data = get_callback_data(button_type);
      if (!td::check_utf8(data)) {
        object("callback_data", "INVALID");
      } else {
        object("callback_data", data);
      }
      break;
    }
    case td_api::inlineKeyboardButtonTypeCallbackGame::ID:
      if (for_rich_message) {
        LOG(ERROR) << "Receive inlineKeyboardButtonTypeCallbackGame";
      }
      object("callback_game", JsonEmptyObject());
      break;
    case td_api::inlineKeyboardButtonTypeSwitchInline::ID: {
      auto type = static_cast<const td_api::inlineKeyboardButtonTypeSwitchInline *>(button_type);
      switch (type->target_chat_->get_id()) {
        case td_api::targetChatCurrent::ID:
          object("switch_inline_query_current_chat", type->query_);
          break;
        case td_api::targetChatChosen::ID: {
          auto types = static_cast<const td_api::targetChatChosen *>(type->target_chat_.get())->types_.get();
          if (types->allow_user_chats_ && types->allow_bot_chats_ && types->allow_group_chats_ &&
              types->allow_channel_chats_) {
            object("switch_inline_query", type->query_);
          } else {
            object("switch_inline_query_chosen_chat", td::json_object([&](auto &o) {
                     o("query", type->query_);
                     o("allow_user_chats", td::JsonBool(types->allow_user_chats_));
                     o("allow_bot_chats", td::JsonBool(types->allow_bot_chats_));
                     o("allow_group_chats", td::JsonBool(types->allow_group_chats_));
                     o("allow_channel_chats", td::JsonBool(types->allow_channel_chats_));
                   }));
          }
          break;
        }
        default:
          UNREACHABLE();
      }
      break;
    }
    case td_api::inlineKeyboardButtonTypeBuy::ID:
      if (for_rich_message) {
        LOG(ERROR) << "Receive inlineKeyboardButtonTypeBuy";
      }
      object("pay", td::JsonTrue());
      break;
    case td_api::inlineKeyboardButtonTypeUser::ID: {
      auto type = static_cast<const td_api::inlineKeyboardButtonTypeUser *>(button_type);
      object("url", PSLICE() << "tg://user?id=" << type->user_id_);
      break;
    }
    case td_api::inlineKeyboardButtonTypeWebApp::ID: {
      auto type = static_cast<const td_api::inlineKeyboardButtonTypeWebApp *>(button_type);
      object("web_app", JsonWebAppInfo(type->url_));
      break;
    }
    case td_api::inlineKeyboardButtonTypeCopyText::ID: {
      auto type = static_cast<const td_api::inlineKeyboardButtonTypeCopyText *>(button_type);
      object("copy_text", JsonCopyTextButton(type->text_));
      break;
    }
    case td_api::inlineKeyboardButtonTypeDisabled::ID:
      object("disabled", JsonEmptyObject());
      break;
    default:
      UNREACHABLE();
      break;
  }
}

void Client::json_store_horizontal_alignment(td::JsonObjectScope &object,
                                             const td_api::PageBlockHorizontalAlignment *alignment) {
  CHECK(alignment != nullptr);
  switch (alignment->get_id()) {
    case td_api::pageBlockHorizontalAlignmentLeft::ID:
      object("align", "left");
      break;
    case td_api::pageBlockHorizontalAlignmentCenter::ID:
      object("align", "center");
      break;
    case td_api::pageBlockHorizontalAlignmentRight::ID:
      object("align", "right");
      break;
    default:
      UNREACHABLE();
  }
}

void Client::json_store_message_sender(td::JsonObjectScope &object, const object_ptr<td_api::MessageSender> &sender,
                                       td::Slice user_field_name, td::Slice chat_field_name,
                                       int64 backward_compatibility_user_id) const {
  CHECK(sender != nullptr);
  switch (sender->get_id()) {
    case td_api::messageSenderUser::ID: {
      auto sender_id = static_cast<const td_api::messageSenderUser *>(sender.get());
      object(user_field_name, JsonUser(sender_id->user_id_, this));
      break;
    }
    case td_api::messageSenderChat::ID: {
      auto sender_id = static_cast<const td_api::messageSenderChat *>(sender.get());
      if (backward_compatibility_user_id > 0) {
        object(user_field_name, JsonUser(backward_compatibility_user_id, this));
      }
      object(chat_field_name, JsonChat(sender_id->chat_id_, this));
      break;
    }
    default:
      UNREACHABLE();
  }
}

td::Slice Client::get_update_type_name(UpdateType update_type) {
  switch (update_type) {
    case UpdateType::Message:
      return td::Slice("message");
    case UpdateType::EditedMessage:
      return td::Slice("edited_message");
    case UpdateType::ChannelPost:
      return td::Slice("channel_post");
    case UpdateType::EditedChannelPost:
      return td::Slice("edited_channel_post");
    case UpdateType::InlineQuery:
      return td::Slice("inline_query");
    case UpdateType::ChosenInlineResult:
      return td::Slice("chosen_inline_result");
    case UpdateType::CallbackQuery:
      return td::Slice("callback_query");
    case UpdateType::CustomEvent:
      return td::Slice("custom_event");
    case UpdateType::CustomQuery:
      return td::Slice("custom_query");
    case UpdateType::ShippingQuery:
      return td::Slice("shipping_query");
    case UpdateType::PreCheckoutQuery:
      return td::Slice("pre_checkout_query");
    case UpdateType::Poll:
      return td::Slice("poll");
    case UpdateType::PollAnswer:
      return td::Slice("poll_answer");
    case UpdateType::MyChatMember:
      return td::Slice("my_chat_member");
    case UpdateType::ChatMember:
      return td::Slice("chat_member");
    case UpdateType::ChatJoinRequest:
      return td::Slice("chat_join_request");
    case UpdateType::ChatBoostUpdated:
      return td::Slice("chat_boost");
    case UpdateType::ChatBoostRemoved:
      return td::Slice("removed_chat_boost");
    case UpdateType::MessageReaction:
      return td::Slice("message_reaction");
    case UpdateType::MessageReactionCount:
      return td::Slice("message_reaction_count");
    case UpdateType::BusinessConnection:
      return td::Slice("business_connection");
    case UpdateType::BusinessMessage:
      return td::Slice("business_message");
    case UpdateType::EditedBusinessMessage:
      return td::Slice("edited_business_message");
    case UpdateType::BusinessMessagesDeleted:
      return td::Slice("deleted_business_messages");
    case UpdateType::PurchasedPaidMedia:
      return td::Slice("purchased_paid_media");
    case UpdateType::ManagedBot:
      return td::Slice("managed_bot");
    case UpdateType::GuestMessage:
      return td::Slice("guest_message");
    case UpdateType::Subscription:
      return td::Slice("subscription");
    case UpdateType::StopMessageDraft:
      return td::Slice("stopped_message_generation");
    default:
      UNREACHABLE();
      return td::Slice();
  }
}

td::uint64 Client::get_allowed_update_types(td::MutableSlice allowed_updates, bool is_internal) {
  if (allowed_updates.empty()) {
    return 0;
  }

  LOG(INFO) << "Parsing JSON object: " << allowed_updates;
  auto r_value = json_decode(allowed_updates);
  if (r_value.is_error()) {
    LOG(INFO) << "Can't parse JSON object: " << r_value.error();
    return 0;
  }

  td::uint64 result = 0;
  auto value = r_value.move_as_ok();
  if (value.type() != td::JsonValue::Type::Array) {
    if (value.type() == td::JsonValue::Type::Number && is_internal) {
      auto r_number = td::to_integer_safe<td::uint64>(value.get_number());
      if (r_number.is_ok() && r_number.ok() > 0) {
        return r_number.ok();
      }
    }
    return 0;
  }
  for (auto &update_type_name : value.get_array()) {
    if (update_type_name.type() != td::JsonValue::Type::String) {
      return 0;
    }
    auto type_name = update_type_name.get_string();
    to_lower_inplace(type_name);
    for (int32 i = 0; i < static_cast<int32>(UpdateType::Size); i++) {
      if (get_update_type_name(static_cast<UpdateType>(i)) == type_name) {
        result |= (static_cast<td::uint64>(1) << i);
      }
    }
  }

  if (result == 0) {
    return DEFAULT_ALLOWED_UPDATE_TYPES;
  }
  return result;
}

bool Client::update_allowed_update_types(const Query *query) {
  auto allowed_update_types = get_allowed_update_types(query->arg("allowed_updates"), query->is_internal());
  if (allowed_update_types != 0 && allowed_update_types != allowed_update_types_) {
    allowed_update_types_ = allowed_update_types;
    object_ptr<td_api::OptionValue> value;
    if (allowed_update_types == DEFAULT_ALLOWED_UPDATE_TYPES) {
      value = make_object<td_api::optionValueEmpty>();
    } else {
      value = make_object<td_api::optionValueInteger>(static_cast<int64>(allowed_update_types));
    }
    send_request(make_object<td_api::setOption>("xallowed_update_types", std::move(value)),
                 td::make_unique<TdOnOkCallback>());
    return true;
  }
  return false;
}

template <class T>
class UpdateJsonable final : public td::VirtuallyJsonable {
 public:
  explicit UpdateJsonable(const T &update) : update(update) {
  }
  void store(td::JsonValueScope *scope) const final {
    *scope << update;
  }

 private:
  const T &update;
};

template <class T>
void Client::add_update(UpdateType update_type, const T &update, int32 timeout, int64 webhook_queue_id) {
  add_update_impl(update_type, UpdateJsonable<T>(update), timeout, webhook_queue_id);
}

void Client::add_update_impl(UpdateType update_type, const td::VirtuallyJsonable &update, int32 timeout,
                             int64 webhook_queue_id) {
  update_last_synchronization_error_date();
  last_update_creation_time_ = td::Time::now();

  if (((allowed_update_types_ >> static_cast<int32>(update_type)) & 1) == 0) {
    LOG(DEBUG) << "Skip disallowed update of the type " << static_cast<int32>(update_type)
               << ", allowed update mask is " << allowed_update_types_;
    return;
  }

  send_closure(stat_actor_, &BotStatActor::add_event<ServerBotStat::Update>, ServerBotStat::Update{}, td::Time::now());

  const size_t BUF_SIZE = 1 << 18;
  auto buf = td::StackAllocator::alloc(BUF_SIZE);
  td::JsonBuilder jb(td::StringBuilder(buf.as_slice(), true));
  jb.enter_value() << get_update_type_name(update_type);
  jb.string_builder() << ":";
  jb.enter_value() << update;
  if (jb.string_builder().is_error()) {
    LOG(ERROR) << "JSON buffer overflow";
    return;
  }

  auto update_slice = jb.string_builder().as_cslice();
  auto r_id = parameters_->shared_data_->tqueue_->push(tqueue_id_, update_slice.str(), get_unix_time() + timeout,
                                                       webhook_queue_id, td::TQueue::EventId());
  if (r_id.is_ok()) {
    auto id = r_id.move_as_ok();
    LOG(DEBUG) << "Update " << id << " was added for " << timeout << " seconds: " << update_slice;
    if (webhook_url_.empty()) {
      long_poll_wakeup(false);
    } else {
      send_closure(webhook_id_, &WebhookActor::update);
    }
  } else {
    LOG(DEBUG) << "Update failed to be added with error " << r_id.error() << " for " << timeout
               << " seconds: " << update_slice;
  }
}

void Client::add_new_message(object_ptr<td_api::message> &&message, bool is_edited) {
  CHECK(message != nullptr);

  if (message->sending_state_ != nullptr) {
    return;
  }

  auto chat_id = message->chat_id_;
  CHECK(chat_id != 0);
  new_message_queues_[chat_id].queue_.emplace(create_message(std::move(message)), is_edited);
  process_new_message_queue(chat_id, 0);
}

void Client::add_message_update(UpdateType update_type, const MessageInfo *message_info, int64 webhook_queue_id) {
  int32 message_date = message_info->edit_date == 0 ? message_info->date : message_info->edit_date;
  auto now = get_unix_time();
  auto update_delay_time = now - td::max(message_date, parameters_->shared_data_->get_unix_time(webhook_set_time_));
  const auto UPDATE_DELAY_WARNING_TIME = 10 * 60;
  if (delayed_update_count_ > 0 && (update_type != delayed_update_type_ || message_info->chat_id != delayed_chat_id_)) {
    if (delayed_update_count_ == 1) {
      LOG(ERROR) << "Receive very old update " << get_update_type_name(delayed_update_type_) << " sent at "
                 << delayed_min_date_ << " in chat " << delayed_chat_id_ << " with a delay of " << delayed_max_time_
                 << " seconds";
    } else {
      LOG(ERROR) << "Receive " << delayed_update_count_ << " very old updates "
                 << get_update_type_name(delayed_update_type_) << " sent from " << delayed_min_date_ << " to "
                 << delayed_max_date_ << " in chat " << delayed_chat_id_ << " with a delay up to " << delayed_max_time_
                 << " seconds";
    }
    delayed_update_count_ = 0;
  }
  if (message_date > log_in_date_ && update_delay_time > UPDATE_DELAY_WARNING_TIME &&
      message_date > last_synchronization_error_date_ + 60) {
    if (delayed_update_count_ == 0) {
      delayed_update_type_ = update_type;
      delayed_chat_id_ = message_info->chat_id;
      delayed_min_date_ = message_date;
      delayed_max_date_ = message_date;
      delayed_max_time_ = update_delay_time;
    } else {
      delayed_min_date_ = td::min(message_date, delayed_min_date_);
      delayed_max_date_ = td::max(message_date, delayed_max_date_);
      delayed_max_time_ = td::max(update_delay_time, delayed_max_time_);
    }
    delayed_update_count_++;
  }
  auto left_time = message_date + 86400 - now;

  message_info->is_content_changed = false;
  add_update(update_type, JsonMessage(message_info, true, get_update_type_name(update_type).str(), this), left_time,
             webhook_queue_id);
}

void Client::add_update_poll(object_ptr<td_api::updatePoll> &&update) {
  CHECK(update != nullptr);
  add_update(UpdateType::Poll, JsonPoll(update->poll_.get(), nullptr, nullptr, this), 86400, update->poll_->id_);
}

void Client::add_update_poll_answer(object_ptr<td_api::updatePollAnswer> &&update) {
  CHECK(update != nullptr);
  add_update(UpdateType::PollAnswer, JsonPollAnswer(update.get(), this), 86400, update->poll_id_);
}

void Client::add_new_inline_query(int64 inline_query_id, int64 sender_user_id, object_ptr<td_api::location> location,
                                  object_ptr<td_api::ChatType> chat_type, const td::string &query,
                                  const td::string &offset) {
  add_update(UpdateType::InlineQuery,
             JsonInlineQuery(inline_query_id, sender_user_id, location.get(), chat_type.get(), query, offset, this), 30,
             sender_user_id + (static_cast<int64>(1) << 33));
}

void Client::add_new_chosen_inline_result(int64 sender_user_id, object_ptr<td_api::location> location,
                                          const td::string &query, const td::string &result_id,
                                          const td::string &inline_message_id) {
  add_update(UpdateType::ChosenInlineResult,
             JsonChosenInlineResult(sender_user_id, location.get(), query, result_id, inline_message_id, this), 600,
             sender_user_id + (static_cast<int64>(2) << 33));
}

void Client::add_new_callback_query(object_ptr<td_api::updateNewCallbackQuery> &&query) {
  CHECK(query != nullptr);
  auto user_id = query->sender_user_id_;
  CHECK(user_id != 0);
  new_callback_query_queues_[user_id].queue_.push(std::move(query));
  process_new_callback_query_queue(user_id, 0);
}

void Client::process_new_callback_query_queue(int64 user_id, int state) {
  auto &queue = new_callback_query_queues_[user_id];
  if (queue.has_active_request_) {
    CHECK(!queue.queue_.empty());
    if (state != 0) {
      queue.has_active_request_ = false;
    } else {
      LOG(INFO) << "Have an active request in callback query queue of size " << queue.queue_.size() << " for user "
                << user_id;
      return;
    }
  } else {
    CHECK(state == 0);
  }
  if (logging_out_ || closing_) {
    LOG(INFO) << "Ignore callback query while closing for user " << user_id;
    new_callback_query_queues_.erase(user_id);
    return;
  }
  while (!queue.queue_.empty()) {
    auto &query = queue.queue_.front();
    int64 chat_id = query->chat_id_;
    int64 message_id = query->message_id_;
    auto message_info = get_message(chat_id, message_id, state > 0);
    // callback message can be already deleted in the bot outbox
    LOG(INFO) << "Process callback query from user " << user_id << " in message " << message_id << " in chat "
              << chat_id << " with state " << state;
    if (state == 0) {
      if (message_info == nullptr) {
        // get the message from the server
        queue.has_active_request_ = true;
        return send_request(make_object<td_api::getCallbackQueryMessage>(chat_id, message_id, query->id_),
                            td::make_unique<TdOnGetCallbackQueryMessageCallback>(this, user_id, state));
      }
      state = 1;
    }
    if (state == 1) {
      auto reply_to_message_id = get_same_chat_reply_to_message_id(message_info, false);
      if (reply_to_message_id > 0 && get_message(chat_id, reply_to_message_id, false) == nullptr) {
        queue.has_active_request_ = true;
        return send_request(make_object<td_api::getRepliedMessage>(chat_id, message_id),
                            td::make_unique<TdOnGetCallbackQueryMessageCallback>(this, user_id, state));
      }
      state = 2;
    }
    if (state == 2) {
      auto sticker_set_ids = get_message_sticker_set_ids(message_info);
      auto reply_to_message_id = get_same_chat_reply_to_message_id(message_info, false);
      td::combine(sticker_set_ids, get_message_sticker_set_ids(get_message(chat_id, reply_to_message_id, true)));
      if (!sticker_set_ids.empty()) {
        queue.has_active_request_ = true;
        return get_sticker_set_names(std::move(sticker_set_ids),
                                     td::PromiseCreator::lambda([actor_id = actor_id(this), user_id](td::Unit) {
                                       send_closure(actor_id, &Client::process_new_callback_query_queue, user_id, 3);
                                     }));
      }
      state = 3;
    }
    CHECK(state == 3);

    CHECK(user_id == query->sender_user_id_);
    if (message_info != nullptr || as_tdlib_message_id(as_client_message_id_unchecked(message_id)) == message_id) {
      add_update(UpdateType::CallbackQuery,
                 JsonCallbackQuery(query->id_, user_id, chat_id, message_id, message_info, query->chat_instance_,
                                   query->payload_.get(), this),
                 150, user_id + (static_cast<int64>(3) << 33));
    }
    queue.queue_.pop();
    state = 0;
  }
  new_callback_query_queues_.erase(user_id);
}

void Client::add_new_business_callback_query(object_ptr<td_api::updateNewBusinessCallbackQuery> &&query) {
  CHECK(query != nullptr);
  auto user_id = query->sender_user_id_;
  CHECK(user_id != 0);
  NewBusinessCallbackQuery new_query;
  new_query.message_info_ = create_business_message(query->connection_id_, std::move(query->message_));
  new_query.update_ = std::move(query);
  new_business_callback_query_queues_[user_id].queue_.push(std::move(new_query));
  process_new_business_callback_query_queue(user_id, 0);
}

void Client::process_new_business_callback_query_queue(int64 user_id, int state) {
  auto &queue = new_business_callback_query_queues_[user_id];
  if (queue.has_active_request_) {
    CHECK(!queue.queue_.empty());
    if (state != 0) {
      queue.has_active_request_ = false;
    } else {
      LOG(INFO) << "Have an active request in business callback query queue of size " << queue.queue_.size()
                << " for user " << user_id;
      return;
    }
  } else {
    CHECK(state == 0);
  }
  if (logging_out_ || closing_) {
    LOG(INFO) << "Ignore business callback query while closing for user " << user_id;
    new_business_callback_query_queues_.erase(user_id);
    return;
  }
  while (!queue.queue_.empty()) {
    auto &query = queue.queue_.front();
    LOG(INFO) << "Process business callback query from user " << user_id;

    if (state == 0) {
      auto sticker_set_ids = get_message_sticker_set_ids(query.message_info_.get());
      if (!sticker_set_ids.empty()) {
        queue.has_active_request_ = true;
        return get_sticker_set_names(
            std::move(sticker_set_ids), td::PromiseCreator::lambda([actor_id = actor_id(this), user_id](td::Unit) {
              send_closure(actor_id, &Client::process_new_business_callback_query_queue, user_id, 1);
            }));
      }
      state = 1;
    }
    CHECK(state == 1);

    auto update = std::move(query.update_);
    CHECK(user_id == update->sender_user_id_);
    add_update(UpdateType::CallbackQuery,
               JsonCallbackQuery(update->id_, user_id, 0, 0, query.message_info_.get(), update->chat_instance_,
                                 update->payload_.get(), this),
               150, user_id + (static_cast<int64>(3) << 33));
    queue.queue_.pop();
    state = 0;
  }
  new_business_callback_query_queues_.erase(user_id);
}

void Client::add_new_inline_callback_query(object_ptr<td_api::updateNewInlineCallbackQuery> &&query) {
  CHECK(query != nullptr);
  add_update(UpdateType::CallbackQuery,
             JsonInlineCallbackQuery(query->id_, query->sender_user_id_, query->inline_message_id_,
                                     query->chat_instance_, query->payload_.get(), this),
             150, query->sender_user_id_ + (static_cast<int64>(3) << 33));
}

void Client::add_new_shipping_query(object_ptr<td_api::updateNewShippingQuery> &&query) {
  CHECK(query != nullptr);
  add_update(UpdateType::ShippingQuery, JsonShippingQuery(query.get(), this), 150,
             query->sender_user_id_ + (static_cast<int64>(4) << 33));
}

void Client::add_new_pre_checkout_query(object_ptr<td_api::updateNewPreCheckoutQuery> &&query) {
  CHECK(query != nullptr);
  add_update(UpdateType::PreCheckoutQuery, JsonPreCheckoutQuery(query.get(), this), 150,
             query->sender_user_id_ + (static_cast<int64>(4) << 33));
}

void Client::add_update_purchased_paid_media(object_ptr<td_api::updatePaidMediaPurchased> &&query) {
  CHECK(query != nullptr);
  add_update(UpdateType::PurchasedPaidMedia, JsonPaidMediaPurchased(query.get(), this), 86400,
             query->user_id_ + (static_cast<int64>(12) << 33));
}

void Client::add_update_managed_bot(object_ptr<td_api::updateManagedBot> &&query) {
  CHECK(query != nullptr);
  add_update(UpdateType::ManagedBot, JsonManagedBotUpdated(query.get(), this), 86400,
             query->user_id_ + (static_cast<int64>(13) << 33));
}

void Client::add_update_subscription(object_ptr<td_api::updateUserSubscription> &&query) {
  CHECK(query != nullptr);
  add_update(UpdateType::Subscription, JsonBotSubscriptionUpdated(query.get(), this), 86400,
             query->user_id_ + (static_cast<int64>(14) << 33));
}

void Client::add_update_stopped_message_generation(object_ptr<td_api::updateStopMessageDraft> &&update) {
  CHECK(update != nullptr);
  add_update(UpdateType::StopMessageDraft, JsonMessageGenerationStopped(update.get(), this), 150,
             update->chat_id_ + (static_cast<int64>(15) << 33));
}

void Client::add_new_custom_event(object_ptr<td_api::updateNewCustomEvent> &&event) {
  CHECK(event != nullptr);
  add_update(UpdateType::CustomEvent, JsonCustomJson(event->event_), 600, 0);
}

void Client::add_new_custom_query(object_ptr<td_api::updateNewCustomQuery> &&query) {
  CHECK(query != nullptr);
  int32 timeout = query->timeout_ <= 0 ? 86400 : query->timeout_;
  add_update(UpdateType::CustomQuery, JsonCustomJson(query->data_), timeout, 0);
}

void Client::add_update_chat_member(object_ptr<td_api::updateChatMember> &&update) {
  CHECK(update != nullptr);
  auto left_time = update->date_ + 86400 - get_unix_time();
  if (left_time > 0) {
    CHECK(update->old_chat_member_->member_id_ != nullptr);
    if (update->old_chat_member_->member_id_->get_id() != td_api::messageSenderUser::ID ||
        update->new_chat_member_->member_id_->get_id() != td_api::messageSenderUser::ID) {
      return;
    }
    auto user_id = static_cast<const td_api::messageSenderUser *>(update->old_chat_member_->member_id_.get())->user_id_;
    bool is_my = (user_id == my_id_);
    auto webhook_queue_id = (is_my ? update->chat_id_ : user_id) + (static_cast<int64>(is_my ? 5 : 6) << 33);
    auto update_type = is_my ? UpdateType::MyChatMember : UpdateType::ChatMember;
    add_update(update_type, JsonChatMemberUpdated(update.get(), this), left_time, webhook_queue_id);
  } else {
    LOG(DEBUG) << "Skip updateChatMember with date " << update->date_ << ", because current date is "
               << get_unix_time();
  }
}

void Client::add_update_chat_join_request(object_ptr<td_api::updateNewChatJoinRequest> &&update) {
  CHECK(update != nullptr);
  CHECK(update->request_ != nullptr);
  auto left_time = update->request_->date_ + 86400 - get_unix_time();
  if (left_time > 0) {
    auto webhook_queue_id = update->request_->user_id_ + (static_cast<int64>(6) << 33);
    add_update(UpdateType::ChatJoinRequest, JsonChatJoinRequest(update.get(), this), left_time, webhook_queue_id);
  } else {
    LOG(DEBUG) << "Skip updateNewChatJoinRequest with date " << update->request_->date_ << ", because current date is "
               << get_unix_time();
  }
}

void Client::add_update_chat_boost(object_ptr<td_api::updateChatBoost> &&update) {
  CHECK(update != nullptr);
  auto left_time = update->boost_->start_date_ + 86400 - get_unix_time();
  if (left_time > 0) {
    auto webhook_queue_id = update->chat_id_ + (static_cast<int64>(7) << 33);
    if (update->boost_->expiration_date_ == 0) {
      add_update(UpdateType::ChatBoostRemoved, JsonChatBoostRemoved(update.get(), this), left_time, webhook_queue_id);
    } else {
      add_update(UpdateType::ChatBoostUpdated, JsonChatBoostUpdated(update.get(), this), left_time, webhook_queue_id);
    }
  } else {
    LOG(DEBUG) << "Skip updateChatBoost with date " << update->boost_->start_date_ << ", because current date is "
               << get_unix_time();
  }
}

void Client::add_update_message_reaction(object_ptr<td_api::updateMessageReaction> &&update) {
  CHECK(update != nullptr);
  auto left_time = update->date_ + 86400 - get_unix_time();
  if (left_time > 0) {
    auto webhook_queue_id = update->chat_id_ + (static_cast<int64>(8) << 33);
    add_update(UpdateType::MessageReaction, JsonMessageReactionUpdated(update.get(), this), left_time,
               webhook_queue_id);
  } else {
    LOG(DEBUG) << "Skip updateMessageReaction with date " << update->date_ << ", because current date is "
               << get_unix_time();
  }
}

void Client::add_update_message_reaction_count(object_ptr<td_api::updateMessageReactions> &&update) {
  CHECK(update != nullptr);
  auto left_time = update->date_ + 86400 - get_unix_time();
  if (left_time > 0) {
    auto webhook_queue_id = update->chat_id_ + (static_cast<int64>(9) << 33);
    add_update(UpdateType::MessageReactionCount, JsonMessageReactionCountUpdated(update.get(), this), left_time,
               webhook_queue_id);
  } else {
    LOG(DEBUG) << "Skip updateMessageReactions with date " << update->date_ << ", because current date is "
               << get_unix_time();
  }
}

void Client::add_update_business_connection(object_ptr<td_api::updateBusinessConnection> &&update) {
  CHECK(update != nullptr);
  const auto *connection = add_business_connection(std::move(update->connection_), true);
  auto webhook_queue_id = connection->user_id_ + (static_cast<int64>(10) << 33);
  add_update(UpdateType::BusinessConnection, JsonBusinessConnection(connection, this), 86400, webhook_queue_id);
}

void Client::add_update_business_messages_deleted(object_ptr<td_api::updateBusinessMessagesDeleted> &&update) {
  CHECK(update != nullptr);
  auto webhook_queue_id = update->chat_id_ + (static_cast<int64>(11) << 33);
  add_update(UpdateType::BusinessMessagesDeleted, JsonBusinessMessagesDeleted(update.get(), this), 86400,
             webhook_queue_id);
}

void Client::add_new_business_message(object_ptr<td_api::updateNewBusinessMessage> &&update) {
  CHECK(update != nullptr);
  CHECK(!update->connection_id_.empty());
  auto message_info = create_business_message(update->connection_id_, std::move(update->message_));
  new_business_message_queues_[update->connection_id_].queue_.emplace(std::move(message_info), false);
  process_new_business_message_queue(update->connection_id_, 0);
}

void Client::add_business_message_edited(object_ptr<td_api::updateBusinessMessageEdited> &&update) {
  CHECK(update != nullptr);
  CHECK(!update->connection_id_.empty());
  auto message_info = create_business_message(update->connection_id_, std::move(update->message_));
  new_business_message_queues_[update->connection_id_].queue_.emplace(std::move(message_info), true);
  process_new_business_message_queue(update->connection_id_, 0);
}

void Client::add_new_guest_query(object_ptr<td_api::updateNewGuestQuery> &&update) {
  CHECK(update != nullptr);
  CHECK(update->id_ != 0);
  auto message_info = create_message(std::move(update->message_));
  message_info->reference_messages =
      transform(std::move(update->reference_messages_),
                [&](object_ptr<td_api::message> &&message) { return create_message(std::move(message)); });
  message_info->guest_query_id = update->id_;
  auto chat_id = message_info->chat_id;
  new_guest_query_queues_[chat_id].queue_.emplace(std::move(message_info));
  process_new_guest_query_queue(chat_id, 0);
}

td::int64 Client::choose_added_member_id(const td_api::messageChatAddMembers *message_add_members) const {
  CHECK(message_add_members != nullptr);
  for (auto &member_user_id : message_add_members->member_user_ids_) {
    if (member_user_id == my_id_) {
      return my_id_;
    }
  }
  if (message_add_members->member_user_ids_.empty()) {
    return 0;
  }
  return message_add_members->member_user_ids_[0];
}

bool Client::need_skip_update_message(int64 chat_id, const MessageInfo *message_info, bool is_edited) const {
  const ChatInfo *chat;
  ChatInfo::Type chat_type;
  if (chat_id != 0) {
    chat = get_chat(chat_id);
    CHECK(chat != nullptr);
    chat_type = chat->type;
  } else {
    chat = nullptr;
    chat_type = ChatInfo::Type::Private;
  }
  auto message_content_id = message_info->content->get_id();
  if (message_info->is_outgoing && chat_id != 0) {
    switch (message_content_id) {
      case td_api::messageChatChangeTitle::ID:
      case td_api::messageChatChangePhoto::ID:
      case td_api::messageChatDeletePhoto::ID:
      case td_api::messageChatDeleteMember::ID:
      case td_api::messageChatSetTheme::ID:
      case td_api::messagePinMessage::ID:
      case td_api::messageProximityAlertTriggered::ID:
      case td_api::messageVideoChatScheduled::ID:
      case td_api::messageVideoChatStarted::ID:
      case td_api::messageVideoChatEnded::ID:
      case td_api::messageInviteVideoChatParticipants::ID:
      case td_api::messageForumTopicCreated::ID:
      case td_api::messageForumTopicEdited::ID:
      case td_api::messageForumTopicIsClosedToggled::ID:
      case td_api::messageForumTopicIsHiddenToggled::ID:
      case td_api::messageGiveawayCreated::ID:
      case td_api::messageGiveaway::ID:
      case td_api::messageGiveawayWinners::ID:
      case td_api::messageGiveawayCompleted::ID:
      case td_api::messagePaymentRefunded::ID:
      case td_api::messageSuggestedPostApprovalFailed::ID:
      case td_api::messageSuggestedPostApproved::ID:
      case td_api::messageSuggestedPostDeclined::ID:
      case td_api::messageSuggestedPostPaid::ID:
      case td_api::messageSuggestedPostRefunded::ID:
      case td_api::messagePollOptionAdded::ID:
      case td_api::messagePollOptionDeleted::ID:
        // don't skip
        break;
      default:
        return true;
    }
  }

  int32 message_date = message_info->edit_date == 0 ? message_info->date : message_info->edit_date;
  if (message_date <= get_unix_time() - 86400) {
    // don't send messages received/edited more than 1 day ago
    LOG(DEBUG) << "Skip update about message with date " << message_date << ", because current date is "
               << get_unix_time();
    return true;
  }

  if (chat_type == ChatInfo::Type::Supergroup) {
    CHECK(chat != nullptr);
    auto supergroup_info = get_supergroup_info(chat->supergroup_id);
    if (supergroup_info->status->get_id() == td_api::chatMemberStatusLeft::ID ||
        supergroup_info->status->get_id() == td_api::chatMemberStatusBanned::ID) {
      // if we have left the chat, send only update about leaving the supergroup
      if (message_content_id == td_api::messageChatDeleteMember::ID) {
        auto user_id = static_cast<const td_api::messageChatDeleteMember *>(message_info->content.get())->user_id_;
        return user_id != my_id_;
      }
      return true;
    }

    if (supergroup_info->date > message_date || authorization_date_ > message_date) {
      // don't send messages received before join or getting authorization
      return true;
    }

    if (!supergroup_info->is_supergroup && message_content_id == td_api::messageSupergroupChatCreate::ID) {
      // don't send message about channel creation, even if the bot was added at exactly the same time
      return true;
    }
  }

  if (message_info->is_self_destruct) {
    return true;
  }

  if (message_info->is_imported) {
    return true;
  }

  switch (message_content_id) {
    case td_api::messageChatAddMembers::ID: {
      auto content = static_cast<const td_api::messageChatAddMembers *>(message_info->content.get());
      if (content->member_user_ids_.empty()) {
        LOG(ERROR) << "Receive empty messageChatAddMembers";
        return true;
      }
      break;
    }
    case td_api::messageSupergroupChatCreate::ID: {
      if (chat_type != ChatInfo::Type::Supergroup) {
        LOG(ERROR) << "Receive messageSupergroupChatCreate in the non-supergroup chat " << chat_id;
        return true;
      }
      break;
    }
    case td_api::messagePinMessage::ID: {
      auto content = static_cast<const td_api::messagePinMessage *>(message_info->content.get());
      auto pinned_message_id = content->message_id_;
      if (pinned_message_id <= 0) {
        return true;
      }
      break;
    }
    case td_api::messageProximityAlertTriggered::ID: {
      auto content = static_cast<const td_api::messageProximityAlertTriggered *>(message_info->content.get());
      return content->traveler_id_->get_id() != td_api::messageSenderUser::ID ||
             content->watcher_id_->get_id() != td_api::messageSenderUser::ID;
    }
    case td_api::messageGameScore::ID:
    case td_api::messagePaymentSuccessful::ID:
    case td_api::messagePassportDataSent::ID:
    case td_api::messageCall::ID:
    case td_api::messageUnsupported::ID:
    case td_api::messageContactRegistered::ID:
    case td_api::messageExpiredPhoto::ID:
    case td_api::messageExpiredVideo::ID:
    case td_api::messageExpiredVideoNote::ID:
    case td_api::messageExpiredVoiceNote::ID:
    case td_api::messageCustomServiceAction::ID:
    case td_api::messageChatSetTheme::ID:
    case td_api::messageWebAppDataSent::ID:
    case td_api::messageGiftedPremium::ID:
    case td_api::messageSuggestProfilePhoto::ID:
    case td_api::messagePremiumGiftCode::ID:
    case td_api::messageGiftedStars::ID:
    case td_api::messageGiveawayPrizeStars::ID:
    case td_api::messagePaidMessagesRefunded::ID:
    case td_api::messageGroupCall::ID:
    case td_api::messageGiftedGrams::ID:
    case td_api::messageSuggestBirthdate::ID:
    case td_api::messageUpgradedGiftPurchaseOffer::ID:
    case td_api::messageUpgradedGiftPurchaseOfferRejected::ID:
    case td_api::messageChatHasProtectedContentToggled::ID:
    case td_api::messageChatHasProtectedContentDisableRequested::ID:
      return true;
    default:
      break;
  }

  if (is_edited && chat_id != 0) {
    const MessageInfo *old_message = get_message(chat_id, message_info->id, true);
    if (old_message != nullptr && !old_message->is_content_changed) {
      return true;
    }
  }

  return false;
}

td::int64 Client::get_same_chat_reply_to_message_id(const MessageInfo *message_info, bool only_explicit) const {
  if (message_info == nullptr) {
    return 0;
  }
  auto content_message_id = [&] {
    if (only_explicit) {
      return static_cast<int64>(0);
    }
    auto content = message_info->content.get();
    switch (content->get_id()) {
      case td_api::messagePinMessage::ID:
        return static_cast<const td_api::messagePinMessage *>(content)->message_id_;
      case td_api::messageGameScore::ID:
        return static_cast<const td_api::messageGameScore *>(content)->game_message_id_;
      case td_api::messagePaymentSuccessful::ID: {
        const auto *payment = static_cast<const td_api::messagePaymentSuccessful *>(content);
        return payment->invoice_chat_id_ == message_info->chat_id ? payment->invoice_message_id_
                                                                  : static_cast<int64>(0);
      }
      case td_api::messageChatSetBackground::ID:
        return static_cast<const td_api::messageChatSetBackground *>(content)->old_background_message_id_;
      case td_api::messageGiveawayCompleted::ID:
        return static_cast<const td_api::messageGiveawayCompleted *>(content)->giveaway_message_id_;
      case td_api::messageChecklistTasksDone::ID:
        return static_cast<const td_api::messageChecklistTasksDone *>(content)->checklist_message_id_;
      case td_api::messageChecklistTasksAdded::ID:
        return static_cast<const td_api::messageChecklistTasksAdded *>(content)->checklist_message_id_;
      case td_api::messageSuggestedPostApprovalFailed::ID:
        return static_cast<const td_api::messageSuggestedPostApprovalFailed *>(content)->suggested_post_message_id_;
      case td_api::messageSuggestedPostApproved::ID:
        return static_cast<const td_api::messageSuggestedPostApproved *>(content)->suggested_post_message_id_;
      case td_api::messageSuggestedPostDeclined::ID:
        return static_cast<const td_api::messageSuggestedPostDeclined *>(content)->suggested_post_message_id_;
      case td_api::messageSuggestedPostPaid::ID:
        return static_cast<const td_api::messageSuggestedPostPaid *>(content)->suggested_post_message_id_;
      case td_api::messageSuggestedPostRefunded::ID:
        return static_cast<const td_api::messageSuggestedPostRefunded *>(content)->suggested_post_message_id_;
      case td_api::messageUpgradedGift::ID: {
        auto *origin = static_cast<const td_api::messageUpgradedGift *>(content)->origin_.get();
        if (origin->get_id() == td_api::upgradedGiftOriginUpgrade::ID) {
          return static_cast<const td_api::upgradedGiftOriginUpgrade *>(origin)->gift_message_id_;
        }
        return static_cast<int64>(0);
      }
      case td_api::messageUpgradedGiftPurchaseOfferRejected::ID:
        return static_cast<const td_api::messageUpgradedGiftPurchaseOfferRejected *>(content)->offer_message_id_;
      case td_api::messageChatHasProtectedContentToggled::ID:
        return static_cast<const td_api::messageChatHasProtectedContentToggled *>(content)->request_message_id_;
      case td_api::messagePollOptionAdded::ID:
        return static_cast<const td_api::messagePollOptionAdded *>(content)->poll_message_id_;
      case td_api::messagePollOptionDeleted::ID:
        return static_cast<const td_api::messagePollOptionDeleted *>(content)->poll_message_id_;
      default:
        return static_cast<int64>(0);
    }
  }();
  if (content_message_id != 0) {
    CHECK(message_info->reply_to_message == nullptr);
    CHECK(message_info->reply_to_story == nullptr);
    return content_message_id;
  }
  if (message_info->reply_to_message != nullptr && message_info->reply_to_message->origin_ == nullptr) {
    CHECK(message_info->reply_to_message->message_id_ > 0);
    return message_info->reply_to_message->message_id_;
  }
  return get_implicit_reply_to_message_id(message_info->chat_id, message_info->id, message_info->topic_id);
}

td::Slice Client::get_sticker_type(const object_ptr<td_api::StickerType> &type) {
  CHECK(type != nullptr);
  switch (type->get_id()) {
    case td_api::stickerTypeRegular::ID:
      return td::Slice("regular");
    case td_api::stickerTypeMask::ID:
      return td::Slice("mask");
    case td_api::stickerTypeCustomEmoji::ID:
      return td::Slice("custom_emoji");
    default:
      UNREACHABLE();
      return td::Slice();
  }
}

td::Result<td_api::object_ptr<td_api::StickerType>> Client::get_sticker_type(td::Slice type) {
  if (type.empty() || type == "regular") {
    return make_object<td_api::stickerTypeRegular>();
  }
  if (type == "mask") {
    return make_object<td_api::stickerTypeMask>();
  }
  if (type == "custom_emoji") {
    return make_object<td_api::stickerTypeCustomEmoji>();
  }
  return td::Status::Error(400, "Unsupported sticker type specified");
}

td::CSlice Client::get_callback_data(const td_api::InlineKeyboardButtonType *type) {
  CHECK(type != nullptr);
  switch (type->get_id()) {
    case td_api::inlineKeyboardButtonTypeCallback::ID:
      return static_cast<const td_api::inlineKeyboardButtonTypeCallback *>(type)->data_;
    case td_api::inlineKeyboardButtonTypeCallbackWithPassword::ID:
      return static_cast<const td_api::inlineKeyboardButtonTypeCallbackWithPassword *>(type)->data_;
    default:
      UNREACHABLE();
      return td::CSlice();
  }
}

bool Client::are_equal_suggested_post_prices(const td_api::SuggestedPostPrice *lhs,
                                             const td_api::SuggestedPostPrice *rhs) {
  if (lhs == nullptr) {
    return rhs == nullptr;
  }
  if (rhs == nullptr) {
    return false;
  }
  if (lhs->get_id() != rhs->get_id()) {
    return false;
  }
  switch (lhs->get_id()) {
    case td_api::suggestedPostPriceStar::ID:
      return static_cast<const td_api::suggestedPostPriceStar *>(lhs)->star_count_ ==
             static_cast<const td_api::suggestedPostPriceStar *>(rhs)->star_count_;
    case td_api::suggestedPostPriceGram::ID:
      return static_cast<const td_api::suggestedPostPriceGram *>(lhs)->gram_cent_count_ ==
             static_cast<const td_api::suggestedPostPriceGram *>(rhs)->gram_cent_count_;
    default:
      UNREACHABLE();
      return false;
  }
}

void Client::set_message_suggested_post_info(MessageInfo *message_info,
                                             object_ptr<td_api::suggestedPostInfo> &&suggested_post_info) {
  if (suggested_post_info == nullptr && message_info->suggested_post_info == nullptr) {
    return;
  }
  if (suggested_post_info != nullptr && message_info->suggested_post_info != nullptr &&
      suggested_post_info->send_date_ == message_info->suggested_post_info->send_date_ &&
      suggested_post_info->state_->get_id() == message_info->suggested_post_info->state_->get_id() &&
      are_equal_suggested_post_prices(suggested_post_info->price_.get(),
                                      message_info->suggested_post_info->price_.get())) {
    return;
  }
  message_info->suggested_post_info = std::move(suggested_post_info);
  message_info->is_content_changed = true;
}

bool Client::are_equal_inline_keyboard_buttons(const td_api::inlineKeyboardButton *lhs,
                                               const td_api::inlineKeyboardButton *rhs) {
  CHECK(lhs != nullptr);
  CHECK(rhs != nullptr);
  if (lhs->text_ != rhs->text_) {
    return false;
  }
  if (lhs->icon_custom_emoji_id_ != rhs->icon_custom_emoji_id_) {
    return false;
  }
  if (lhs->style_->get_id() != rhs->style_->get_id()) {
    return false;
  }
  if (lhs->type_->get_id() != rhs->type_->get_id()) {
    return false;
  }
  switch (lhs->type_->get_id()) {
    case td_api::inlineKeyboardButtonTypeUrl::ID: {
      auto lhs_type = static_cast<const td_api::inlineKeyboardButtonTypeUrl *>(lhs->type_.get());
      auto rhs_type = static_cast<const td_api::inlineKeyboardButtonTypeUrl *>(rhs->type_.get());
      return lhs_type->url_ == rhs_type->url_;
    }
    case td_api::inlineKeyboardButtonTypeLoginUrl::ID: {
      auto lhs_type = static_cast<const td_api::inlineKeyboardButtonTypeLoginUrl *>(lhs->type_.get());
      auto rhs_type = static_cast<const td_api::inlineKeyboardButtonTypeLoginUrl *>(rhs->type_.get());
      return lhs_type->url_ == rhs_type->url_;  // don't compare id_ and forward_text_
    }
    case td_api::inlineKeyboardButtonTypeCallback::ID:
    case td_api::inlineKeyboardButtonTypeCallbackWithPassword::ID:
      return get_callback_data(lhs->type_.get()) == get_callback_data(rhs->type_.get());
    case td_api::inlineKeyboardButtonTypeCallbackGame::ID:
      return true;
    case td_api::inlineKeyboardButtonTypeSwitchInline::ID: {
      auto lhs_type = static_cast<const td_api::inlineKeyboardButtonTypeSwitchInline *>(lhs->type_.get());
      auto rhs_type = static_cast<const td_api::inlineKeyboardButtonTypeSwitchInline *>(rhs->type_.get());
      return lhs_type->query_ == rhs_type->query_ &&
             to_string(lhs_type->target_chat_) == to_string(rhs_type->target_chat_);
    }
    case td_api::inlineKeyboardButtonTypeBuy::ID:
      return true;
    case td_api::inlineKeyboardButtonTypeUser::ID: {
      auto lhs_type = static_cast<const td_api::inlineKeyboardButtonTypeUser *>(lhs->type_.get());
      auto rhs_type = static_cast<const td_api::inlineKeyboardButtonTypeUser *>(rhs->type_.get());
      return lhs_type->user_id_ == rhs_type->user_id_;
    }
    case td_api::inlineKeyboardButtonTypeWebApp::ID: {
      auto lhs_type = static_cast<const td_api::inlineKeyboardButtonTypeWebApp *>(lhs->type_.get());
      auto rhs_type = static_cast<const td_api::inlineKeyboardButtonTypeWebApp *>(rhs->type_.get());
      return lhs_type->url_ == rhs_type->url_;
    }
    case td_api::inlineKeyboardButtonTypeCopyText::ID: {
      auto lhs_type = static_cast<const td_api::inlineKeyboardButtonTypeCopyText *>(lhs->type_.get());
      auto rhs_type = static_cast<const td_api::inlineKeyboardButtonTypeCopyText *>(rhs->type_.get());
      return lhs_type->text_ == rhs_type->text_;
    }
    case td_api::inlineKeyboardButtonTypeDisabled::ID:
      return true;
    default:
      UNREACHABLE();
      return false;
  }
}

bool Client::are_equal_inline_keyboards(const td_api::replyMarkupInlineKeyboard *lhs,
                                        const td_api::replyMarkupInlineKeyboard *rhs) {
  CHECK(lhs != nullptr);
  CHECK(rhs != nullptr);
  auto &old_rows = lhs->rows_;
  auto &new_rows = rhs->rows_;
  if (old_rows.size() != new_rows.size()) {
    return false;
  }
  for (size_t i = 0; i < old_rows.size(); i++) {
    if (old_rows[i].size() != new_rows[i].size()) {
      return false;
    }
    for (size_t j = 0; j < old_rows[i].size(); j++) {
      if (!are_equal_inline_keyboard_buttons(old_rows[i][j].get(), new_rows[i][j].get())) {
        return false;
      }
    }
  }
  return true;
}

void Client::set_message_reply_markup(MessageInfo *message_info, object_ptr<td_api::ReplyMarkup> &&reply_markup) {
  if (reply_markup != nullptr && reply_markup->get_id() != td_api::replyMarkupInlineKeyboard::ID) {
    reply_markup = nullptr;
  }
  if (reply_markup == nullptr && message_info->reply_markup == nullptr) {
    return;
  }
  if (reply_markup != nullptr && message_info->reply_markup != nullptr) {
    CHECK(message_info->reply_markup->get_id() == td_api::replyMarkupInlineKeyboard::ID);
    if (are_equal_inline_keyboards(
            static_cast<const td_api::replyMarkupInlineKeyboard *>(message_info->reply_markup.get()),
            static_cast<const td_api::replyMarkupInlineKeyboard *>(reply_markup.get()))) {
      return;
    }
  }
  message_info->reply_markup = std::move(reply_markup);
  message_info->is_content_changed = true;
}

td::vector<td::int64> Client::get_gift_sticker_set_ids(const object_ptr<td_api::gift> &gift) {
  if (gift->sticker_->set_id_ != 0) {
    return {gift->sticker_->set_id_};
  }
  return {};
}

td::vector<td::int64> Client::get_upgraded_gift_sticker_set_ids(const object_ptr<td_api::upgradedGift> &gift) {
  td::vector<int64> sticker_set_ids;
  if (gift->model_->sticker_->set_id_ != 0) {
    sticker_set_ids.push_back(gift->model_->sticker_->set_id_);
  }
  if (gift->symbol_->sticker_->set_id_ != 0) {
    sticker_set_ids.push_back(gift->symbol_->sticker_->set_id_);
  }
  return sticker_set_ids;
}

td::vector<td::int64> Client::get_sent_gift_sticker_set_ids(const object_ptr<td_api::SentGift> &gift) {
  switch (gift->get_id()) {
    case td_api::sentGiftRegular::ID:
      return get_gift_sticker_set_ids(static_cast<const td_api::sentGiftRegular *>(gift.get())->gift_);
    case td_api::sentGiftUpgraded::ID:
      return get_upgraded_gift_sticker_set_ids(static_cast<const td_api::sentGiftUpgraded *>(gift.get())->gift_);
    default:
      UNREACHABLE();
      return {};
  }
}

td::vector<td::int64> Client::get_message_content_sticker_set_ids(const object_ptr<td_api::MessageContent> &content) {
  CHECK(content != nullptr);
  switch (content->get_id()) {
    case td_api::messageGift::ID:
      return get_gift_sticker_set_ids(static_cast<const td_api::messageGift *>(content.get())->gift_);
    case td_api::messagePoll::ID: {
      const auto *poll = static_cast<const td_api::messagePoll *>(content.get())->poll_.get();
      td::vector<int64> sticker_set_ids;
      for (const auto &option : poll->options_) {
        if (option->media_ != nullptr) {
          td::combine(sticker_set_ids, get_poll_media_sticker_set_ids(option->media_));
        }
      }
      return sticker_set_ids;
    }
    case td_api::messageSticker::ID: {
      auto sticker_set_id = static_cast<const td_api::messageSticker *>(content.get())->sticker_->set_id_;
      if (sticker_set_id != 0) {
        return {sticker_set_id};
      }
      break;
    }
    case td_api::messageUpgradedGift::ID:
      return get_upgraded_gift_sticker_set_ids(static_cast<const td_api::messageUpgradedGift *>(content.get())->gift_);
    case td_api::messageRefundedUpgradedGift::ID:
      return get_gift_sticker_set_ids(static_cast<const td_api::messageRefundedUpgradedGift *>(content.get())->gift_);
    case td_api::messageUpgradedGiftPurchaseOffer::ID:
      return get_upgraded_gift_sticker_set_ids(
          static_cast<const td_api::messageUpgradedGiftPurchaseOffer *>(content.get())->gift_);
    case td_api::messageUpgradedGiftPurchaseOfferRejected::ID:
      return get_upgraded_gift_sticker_set_ids(
          static_cast<const td_api::messageUpgradedGiftPurchaseOfferRejected *>(content.get())->gift_);
    default:
      break;
  }
  return {};
}

td::vector<td::int64> Client::get_poll_media_sticker_set_ids(const object_ptr<td_api::PollMedia> &media) {
  CHECK(media != nullptr);
  switch (media->get_id()) {
    case td_api::pollMediaSticker::ID: {
      auto sticker_set_id = static_cast<const td_api::pollMediaSticker *>(media.get())->sticker_->set_id_;
      if (sticker_set_id != 0) {
        return {sticker_set_id};
      }
      break;
    }
    default:
      break;
  }
  return {};
}

td::vector<td::int64> Client::get_message_sticker_set_ids(const MessageInfo *message_info) {
  if (message_info == nullptr) {
    return {};
  }

  td::vector<int64> sticker_set_ids;
  td::combine(sticker_set_ids, get_message_content_sticker_set_ids(message_info->content));
  if (message_info->reply_to_message != nullptr && message_info->reply_to_message->content_ != nullptr) {
    td::combine(sticker_set_ids, get_message_content_sticker_set_ids(message_info->reply_to_message->content_));
  }
  if (message_info->business_reply_to_message != nullptr) {
    td::combine(sticker_set_ids, get_message_sticker_set_ids(message_info->business_reply_to_message.get()));
  }
  for (const auto &reference_message : message_info->reference_messages) {
    td::combine(sticker_set_ids, get_message_sticker_set_ids(reference_message.get()));
  }
  return sticker_set_ids;
}

td::string Client::get_sticker_set_name(int64 sticker_set_id) const {
  return sticker_set_names_.get(sticker_set_id);
}

void Client::process_new_message_queue(int64 chat_id, int state) {
  auto &queue = new_message_queues_[chat_id];
  if (queue.has_active_request_) {
    if (state != 0) {
      queue.has_active_request_ = false;
    } else {
      return;
    }
  } else {
    CHECK(state == 0);
  }
  if (logging_out_ || closing_) {
    new_message_queues_.erase(chat_id);
    return;
  }
  while (!queue.queue_.empty()) {
    auto &message_info_ref = queue.queue_.front().message_info_;
    CHECK(chat_id == message_info_ref->chat_id);
    int64 message_id = message_info_ref->id;

    int64 reply_to_message_id = get_same_chat_reply_to_message_id(message_info_ref.get(), false);
    if (state == 0) {
      if (reply_to_message_id > 0 && get_message(chat_id, reply_to_message_id, false) == nullptr) {
        queue.has_active_request_ = true;
        return send_request(make_object<td_api::getRepliedMessage>(chat_id, message_id),
                            td::make_unique<TdOnGetReplyMessageCallback>(this, chat_id));
      }
      state = 1;
    }
    if (state == 1) {
      auto sticker_set_ids = get_message_sticker_set_ids(message_info_ref.get());
      td::combine(sticker_set_ids, get_message_sticker_set_ids(get_message(chat_id, reply_to_message_id, true)));
      if (!sticker_set_ids.empty()) {
        queue.has_active_request_ = true;
        return get_sticker_set_names(std::move(sticker_set_ids),
                                     td::PromiseCreator::lambda([actor_id = actor_id(this), chat_id](td::Unit) {
                                       send_closure(actor_id, &Client::process_new_message_queue, chat_id, 2);
                                     }));
      }
      state = 2;
    }
    CHECK(state == 2);

    const auto *message_info = add_message(std::move(message_info_ref), false);
    auto is_edited = queue.queue_.front().is_edited_;
    queue.queue_.pop();
    state = 0;
    if (need_skip_update_message(chat_id, message_info, is_edited)) {
      continue;
    }

    UpdateType update_type;
    auto chat = get_chat(chat_id);
    CHECK(chat != nullptr);
    if (chat->type == ChatInfo::Type::Supergroup && !get_supergroup_info(chat->supergroup_id)->is_supergroup) {
      update_type = is_edited ? UpdateType::EditedChannelPost : UpdateType::ChannelPost;
    } else {
      update_type = is_edited ? UpdateType::EditedMessage : UpdateType::Message;
    }

    add_message_update(update_type, message_info, chat_id);
  }
  new_message_queues_.erase(chat_id);
}

void Client::process_new_business_message_queue(const td::string &connection_id, int state) {
  auto &queue = new_business_message_queues_[connection_id];
  if (queue.has_active_request_) {
    if (state != 0) {
      queue.has_active_request_ = false;
    } else {
      return;
    }
  } else {
    CHECK(state == 0);
  }
  if (logging_out_ || closing_) {
    new_business_message_queues_.erase(connection_id);
    return;
  }
  while (!queue.queue_.empty()) {
    auto &message_info_ref = queue.queue_.front().message_info_;

    if (state == 0) {
      auto sticker_set_ids = get_message_sticker_set_ids(message_info_ref.get());
      if (!sticker_set_ids.empty()) {
        queue.has_active_request_ = true;
        return get_sticker_set_names(std::move(sticker_set_ids),
                                     td::PromiseCreator::lambda([actor_id = actor_id(this), connection_id](td::Unit) {
                                       send_closure(actor_id, &Client::process_new_business_message_queue,
                                                    connection_id, 1);
                                     }));
      }
      state = 1;
    }
    CHECK(state == 1);

    auto message_info = std::move(message_info_ref);
    auto is_edited = queue.queue_.front().is_edited_;
    queue.queue_.pop();

    state = 0;
    if (need_skip_update_message(0, message_info.get(), is_edited)) {
      continue;
    }

    auto webhook_queue_id = message_info->chat_id + (static_cast<int64>(11) << 33);
    auto update_type = is_edited ? UpdateType::EditedBusinessMessage : UpdateType::BusinessMessage;
    add_message_update(update_type, message_info.get(), webhook_queue_id);
  }
  new_business_message_queues_.erase(connection_id);
}

void Client::process_new_guest_query_queue(int64 chat_id, int state) {
  auto &queue = new_guest_query_queues_[chat_id];
  if (queue.has_active_request_) {
    if (state != 0) {
      queue.has_active_request_ = false;
    } else {
      return;
    }
  } else {
    CHECK(state == 0);
  }
  if (logging_out_ || closing_) {
    new_guest_query_queues_.erase(chat_id);
    return;
  }
  while (!queue.queue_.empty()) {
    auto &query = queue.queue_.front();
    if (state == 0) {
      auto sticker_set_ids = get_message_sticker_set_ids(query.message_info_.get());
      if (!sticker_set_ids.empty()) {
        queue.has_active_request_ = true;
        return get_sticker_set_names(std::move(sticker_set_ids),
                                     td::PromiseCreator::lambda([actor_id = actor_id(this), chat_id](td::Unit) {
                                       send_closure(actor_id, &Client::process_new_guest_query_queue, chat_id, 1);
                                     }));
      }
      state = 1;
    }
    CHECK(state == 1);

    auto message_info = std::move(query.message_info_);
    queue.queue_.pop();

    state = 0;
    if (need_skip_update_message(0, message_info.get(), false)) {
      continue;
    }

    auto webhook_queue_id = message_info->chat_id + (static_cast<int64>(12) << 33);
    add_message_update(UpdateType::GuestMessage, message_info.get(), webhook_queue_id);
  }
  new_guest_query_queues_.erase(chat_id);
}

td::unique_ptr<Client::MessageInfo> Client::delete_message(int64 chat_id, int64 message_id, bool only_from_cache) {
  auto message_info = std::move(messages_[{chat_id, message_id}]);
  if (message_info == nullptr) {
    if (yet_unsent_messages_.count({chat_id, message_id}) > 0) {
      // yet unsent message is deleted, possible only if we are trying to write to inaccessible supergroup or
      // sent message was deleted before added to the chat
      auto chat_info = get_chat(chat_id);
      CHECK(chat_info != nullptr);

      auto error = make_object<td_api::error>(
          500, "Internal Server Error: sent message was immediately deleted and can't be returned");
      if (chat_info->type == ChatInfo::Type::Supergroup) {
        auto supergroup_info = get_supergroup_info(chat_info->supergroup_id);
        CHECK(supergroup_info != nullptr);
        if (supergroup_info->status->get_id() == td_api::chatMemberStatusBanned::ID ||
            supergroup_info->status->get_id() == td_api::chatMemberStatusLeft::ID) {
          if (supergroup_info->is_supergroup) {
            error = make_object<td_api::error>(403, "Forbidden: bot is not a member of the supergroup chat");
          } else {
            error = make_object<td_api::error>(403, "Forbidden: bot is not a member of the channel chat");
          }
        }
      }

      on_message_send_failed(chat_id, message_id, 0, std::move(error));
    }
  } else {
    messages_.erase({chat_id, message_id});
  }
  return message_info;
}

const Client::MessageInfo *Client::add_message(object_ptr<td_api::message> &&message, bool force_update_content) {
  return add_message(create_message(std::move(message)), force_update_content);
}

const Client::MessageInfo *Client::add_message(td::unique_ptr<MessageInfo> &&new_message_info,
                                               bool force_update_content) {
  LOG(DEBUG) << "Add message " << new_message_info->id << " to chat " << new_message_info->chat_id;
  auto &message_info = messages_[{new_message_info->chat_id, new_message_info->id}];
  if (message_info != nullptr) {
    new_message_info->is_content_changed = force_update_content || message_info->is_content_changed ||
                                           message_info->sender_tag != new_message_info->sender_tag;
    if (!force_update_content && new_message_info->content->get_id() != td_api::messagePoll::ID) {
      // keep the unchanged old content
      new_message_info->content = std::move(message_info->content);
    }
    std::swap(message_info->suggested_post_info, new_message_info->suggested_post_info);
    set_message_suggested_post_info(new_message_info.get(), std::move(message_info->suggested_post_info));
    std::swap(message_info->reply_markup, new_message_info->reply_markup);
    set_message_reply_markup(new_message_info.get(), std::move(message_info->reply_markup));
  }
  message_info = std::move(new_message_info);

  return message_info.get();
}

td::unique_ptr<Client::MessageInfo> Client::create_message(object_ptr<td_api::message> message) {
  CHECK(message != nullptr);
  CHECK(message->sending_state_ == nullptr);
  auto message_info = td::make_unique<MessageInfo>();
  int64 chat_id = message->chat_id_;
  message_info->id = message->id_;
  message_info->chat_id = chat_id;
  message_info->receiver_id = std::move(message->receiver_id_);
  message_info->ephemeral_message_id = message->ephemeral_message_id_;
  message_info->date = message->date_;
  message_info->edit_date = message->edit_date_;
  message_info->media_album_id = message->media_album_id_;
  message_info->via_bot_user_id = message->via_bot_user_id_;
  message_info->guest_bot_caller_id = std::move(message->guest_bot_caller_id_);
  message_info->sender_business_bot_user_id = message->sender_business_bot_user_id_;

  if (message->forward_info_ != nullptr) {
    message_info->initial_send_date = message->forward_info_->date_;
    message_info->forward_origin = std::move(message->forward_info_->origin_);
    message_info->is_automatic_forward = message->forward_info_->source_ != nullptr &&
                                         get_chat_type(chat_id) == ChatType::Supergroup &&
                                         get_chat_type(message->forward_info_->source_->chat_id_) == ChatType::Channel;
  } else if (message->import_info_ != nullptr) {
    message_info->initial_send_date = message->import_info_->date_;
    message_info->forward_origin = make_object<td_api::messageOriginHiddenUser>(message->import_info_->sender_name_);
  }

  CHECK(message->sender_id_ != nullptr);
  switch (message->sender_id_->get_id()) {
    case td_api::messageSenderUser::ID: {
      auto sender_id = move_object_as<td_api::messageSenderUser>(message->sender_id_);
      message_info->sender_user_id = sender_id->user_id_;
      CHECK(message_info->sender_user_id > 0);
      break;
    }
    case td_api::messageSenderChat::ID: {
      auto sender_id = move_object_as<td_api::messageSenderChat>(message->sender_id_);
      message_info->sender_chat_id = sender_id->chat_id_;

      auto chat_type = get_chat_type(chat_id);
      if (chat_type != ChatType::Channel) {
        if (message_info->sender_chat_id == chat_id) {
          message_info->sender_user_id = group_anonymous_bot_user_id_;
        } else if (message_info->is_automatic_forward) {
          message_info->sender_user_id = service_notifications_user_id_;
        } else {
          message_info->sender_user_id = channel_bot_user_id_;
        }
        CHECK(message_info->sender_user_id > 0);
      }
      break;
    }
    default:
      UNREACHABLE();
  }

  message_info->can_be_saved = message->can_be_saved_;
  message_info->is_scheduled = message->scheduling_state_ != nullptr;
  message_info->is_from_offline = message->is_from_offline_;
  message_info->topic_id = std::move(message->topic_id_);
  message_info->author_signature = std::move(message->author_signature_);
  message_info->sender_boost_count = message->sender_boost_count_;
  message_info->sender_tag = std::move(message->sender_tag_);
  message_info->paid_message_star_count = message->paid_message_star_count_;
  message_info->effect_id = message->effect_id_;
  message_info->is_paid_post = message->is_paid_star_suggested_post_ || message->is_paid_gram_suggested_post_;
  message_info->is_outgoing = message->is_outgoing_;
  message_info->is_self_destruct = message->self_destruct_type_ != nullptr;
  message_info->is_imported = message->import_info_ != nullptr;

  if (message->reply_to_ != nullptr) {
    switch (message->reply_to_->get_id()) {
      case td_api::messageReplyToMessage::ID: {
        message_info->reply_to_message = move_object_as<td_api::messageReplyToMessage>(message->reply_to_);
        auto reply_in_chat_id = message_info->reply_to_message->chat_id_;
        if (reply_in_chat_id != message_info->chat_id && message_info->reply_to_message->origin_ == nullptr) {
          LOG(ERROR) << "Drop reply to message " << message_info->id << " in chat " << message_info->chat_id;
          message_info->reply_to_message = nullptr;
        }
        break;
      }
      case td_api::messageReplyToStory::ID:
        message_info->reply_to_story = move_object_as<td_api::messageReplyToStory>(message->reply_to_);
        break;
      default:
        UNREACHABLE();
    }
  }

  message_info->content = std::move(message->content_);
  set_message_suggested_post_info(message_info.get(), std::move(message->suggested_post_info_));
  set_message_reply_markup(message_info.get(), std::move(message->reply_markup_));

  auto sticker_set_ids = get_message_sticker_set_ids(message_info.get());
  if (!sticker_set_ids.empty()) {
    get_sticker_set_names(std::move(sticker_set_ids), td::Promise<td::Unit>());
  }
  return message_info;
}

td::unique_ptr<Client::MessageInfo> Client::create_business_message(td::string business_connection_id,
                                                                    object_ptr<td_api::businessMessage> &&message) {
  auto message_info = create_message(std::move(message->message_));
  message_info->business_connection_id = std::move(business_connection_id);
  if (message->reply_to_message_ != nullptr) {
    message_info->business_reply_to_message = create_message(std::move(message->reply_to_message_));
    message_info->business_reply_to_message->business_connection_id = message_info->business_connection_id;
  }
  return message_info;
}

void Client::update_message_content(int64 chat_id, int64 message_id, object_ptr<td_api::MessageContent> &&content) {
  auto message_info = get_message_editable(chat_id, message_id);
  if (message_info == nullptr) {
    return;
  }
  LOG(DEBUG) << "Update content of the message " << message_id << " from chat " << chat_id;

  message_info->content = std::move(content);
  message_info->is_content_changed = true;
}

void Client::on_update_message_suggested_post_info(int64 chat_id, int64 message_id,
                                                   object_ptr<td_api::suggestedPostInfo> &&suggested_post_info) {
  auto message_info = get_message_editable(chat_id, message_id);
  if (message_info == nullptr) {
    return;
  }

  set_message_suggested_post_info(message_info, std::move(suggested_post_info));
}

void Client::on_update_message_edited(int64 chat_id, int64 message_id, int32 edit_date,
                                      object_ptr<td_api::ReplyMarkup> &&reply_markup) {
  auto message_info = get_message_editable(chat_id, message_id);
  if (message_info == nullptr) {
    return;
  }
  message_info->edit_date = edit_date;
  set_message_reply_markup(message_info, std::move(reply_markup));
}

const Client::MessageInfo *Client::get_message(int64 chat_id, int64 message_id, bool force_cache) const {
  if (message_id == 0) {
    return nullptr;
  }
  auto message_info = messages_.get_pointer({chat_id, message_id});
  if (message_info == nullptr) {
    LOG(DEBUG) << "Not found message " << message_id << " from chat " << chat_id;
    return nullptr;
  }
  if (!force_cache && message_info->content->get_id() == td_api::messagePoll::ID) {
    LOG(DEBUG) << "Ignore found message " << message_id << " from chat " << chat_id;
    return nullptr;
  }

  LOG(DEBUG) << "Found message " << message_id << " from chat " << chat_id;
  return message_info;
}

Client::MessageInfo *Client::get_message_editable(int64 chat_id, int64 message_id) {
  auto message_info = messages_.get_pointer({chat_id, message_id});
  if (message_info == nullptr) {
    LOG(DEBUG) << "Not found message " << message_id << " from chat " << chat_id;
    return nullptr;
  }
  LOG(DEBUG) << "Found message " << message_id << " from chat " << chat_id;

  return message_info;
}

td::string Client::get_chat_member_status(const object_ptr<td_api::ChatMemberStatus> &status) {
  CHECK(status != nullptr);
  switch (status->get_id()) {
    case td_api::chatMemberStatusCreator::ID:
      return "creator";
    case td_api::chatMemberStatusAdministrator::ID:
      return "administrator";
    case td_api::chatMemberStatusMember::ID:
      return "member";
    case td_api::chatMemberStatusRestricted::ID:
      return "restricted";
    case td_api::chatMemberStatusLeft::ID:
      return "left";
    case td_api::chatMemberStatusBanned::ID:
      return "kicked";
    default:
      UNREACHABLE();
      return "";
  }
}

td::string Client::get_date_time_format(const object_ptr<td_api::DateTimeFormattingType> &formatting_type) {
  if (formatting_type == nullptr) {
    return td::string();
  }
  switch (formatting_type->get_id()) {
    case td_api::dateTimeFormattingTypeRelative::ID:
      return "r";
    case td_api::dateTimeFormattingTypeAbsolute::ID: {
      td::string format;
      auto abs = static_cast<const td_api::dateTimeFormattingTypeAbsolute *>(formatting_type.get());
      if (abs->show_day_of_week_) {
        format += 'w';
      }
      switch (abs->date_precision_->get_id()) {
        case td_api::dateTimePartPrecisionNone::ID:
          break;
        case td_api::dateTimePartPrecisionShort::ID:
          format += 'd';
          break;
        case td_api::dateTimePartPrecisionLong::ID:
          format += 'D';
          break;
        default:
          UNREACHABLE();
      }
      switch (abs->time_precision_->get_id()) {
        case td_api::dateTimePartPrecisionNone::ID:
          break;
        case td_api::dateTimePartPrecisionShort::ID:
          format += 't';
          break;
        case td_api::dateTimePartPrecisionLong::ID:
          format += 'T';
          break;
        default:
          UNREACHABLE();
      }
      return format;
    }
    default:
      UNREACHABLE();
      return {};
  }
}

td::Result<td_api::object_ptr<td_api::DateTimeFormattingType>> Client::get_date_time_formatting_type(td::Slice format) {
  if (format.empty()) {
    return nullptr;
  }
  if (format == "r" || format == "R") {
    return make_object<td_api::dateTimeFormattingTypeRelative>();
  }
  auto result = make_object<td_api::dateTimeFormattingTypeAbsolute>();
  for (auto c : format) {
    switch (c) {
      case 't':
        result->time_precision_ = make_object<td_api::dateTimePartPrecisionShort>();
        break;
      case 'T':
        result->time_precision_ = make_object<td_api::dateTimePartPrecisionLong>();
        break;
      case 'd':
        result->date_precision_ = make_object<td_api::dateTimePartPrecisionShort>();
        break;
      case 'D':
        result->date_precision_ = make_object<td_api::dateTimePartPrecisionLong>();
        break;
      case 'w':
      case 'W':
        result->show_day_of_week_ = true;
        break;
      default:
        return td::Status::Error(400, "Invalid date-time format specified");
    }
  }
  return std::move(result);
}

td::string Client::get_passport_element_type(int32 id) {
  switch (id) {
    case td_api::passportElementTypePersonalDetails::ID:
      return "personal_details";
    case td_api::passportElementTypePassport::ID:
      return "passport";
    case td_api::passportElementTypeDriverLicense::ID:
      return "driver_license";
    case td_api::passportElementTypeIdentityCard::ID:
      return "identity_card";
    case td_api::passportElementTypeInternalPassport::ID:
      return "internal_passport";
    case td_api::passportElementTypeAddress::ID:
      return "address";
    case td_api::passportElementTypeUtilityBill::ID:
      return "utility_bill";
    case td_api::passportElementTypeBankStatement::ID:
      return "bank_statement";
    case td_api::passportElementTypeRentalAgreement::ID:
      return "rental_agreement";
    case td_api::passportElementTypePassportRegistration::ID:
      return "passport_registration";
    case td_api::passportElementTypeTemporaryRegistration::ID:
      return "temporary_registration";
    case td_api::passportElementTypePhoneNumber::ID:
      return "phone_number";
    case td_api::passportElementTypeEmailAddress::ID:
      return "email";
    default:
      UNREACHABLE();
      return "None";
  }
}

td_api::object_ptr<td_api::PassportElementType> Client::get_passport_element_type(td::Slice type) {
  if (type == "personal_details") {
    return make_object<td_api::passportElementTypePersonalDetails>();
  }
  if (type == "passport") {
    return make_object<td_api::passportElementTypePassport>();
  }
  if (type == "driver_license") {
    return make_object<td_api::passportElementTypeDriverLicense>();
  }
  if (type == "identity_card") {
    return make_object<td_api::passportElementTypeIdentityCard>();
  }
  if (type == "internal_passport") {
    return make_object<td_api::passportElementTypeInternalPassport>();
  }
  if (type == "address") {
    return make_object<td_api::passportElementTypeAddress>();
  }
  if (type == "utility_bill") {
    return make_object<td_api::passportElementTypeUtilityBill>();
  }
  if (type == "bank_statement") {
    return make_object<td_api::passportElementTypeBankStatement>();
  }
  if (type == "rental_agreement") {
    return make_object<td_api::passportElementTypeRentalAgreement>();
  }
  if (type == "passport_registration") {
    return make_object<td_api::passportElementTypePassportRegistration>();
  }
  if (type == "temporary_registration") {
    return make_object<td_api::passportElementTypeTemporaryRegistration>();
  }
  if (type == "phone_number") {
    return make_object<td_api::passportElementTypePhoneNumber>();
  }
  if (type == "email") {
    return make_object<td_api::passportElementTypeEmailAddress>();
  }
  return nullptr;
}

td::int32 Client::get_unix_time() const {
  CHECK(was_authorized_);
  return parameters_->shared_data_->get_unix_time(td::Time::now());
}

td::int64 Client::get_implicit_reply_to_message_id(int64 chat_id, int64 message_id,
                                                   const object_ptr<td_api::MessageTopic> &topic_id) const {
  if (topic_id == nullptr) {
    return 0;
  }
  int64 implicit_reply_to_message_id = 0;
  switch (topic_id->get_id()) {
    case td_api::messageTopicThread::ID:
      implicit_reply_to_message_id =
          static_cast<const td_api::messageTopicThread *>(topic_id.get())->message_thread_id_;
      break;
    case td_api::messageTopicForum::ID: {
      auto forum_topic_id = static_cast<const td_api::messageTopicForum *>(topic_id.get())->forum_topic_id_;
      auto chat_info = get_chat(chat_id);
      if (chat_info->type != ChatInfo::Type::Supergroup || forum_topic_id == GENERAL_FORUM_TOPIC_ID) {
        return 0;
      }
      implicit_reply_to_message_id = as_tdlib_message_id(forum_topic_id);
      break;
    }
    case td_api::messageTopicDirectMessages::ID:
      return 0;
    case td_api::messageTopicSavedMessages::ID:
      return 0;
    default:
      UNREACHABLE();
      return 0;
  }
  return implicit_reply_to_message_id < message_id ? implicit_reply_to_message_id : 0;
}

const Client::MessageInfo *Client::get_reply_to_message_info(const MessageInfo *message_info, int64 message_id) const {
  if (!message_info->business_connection_id.empty()) {
    return message_info->business_reply_to_message.get();
  }
  if (message_info->guest_query_id != 0) {
    for (const auto &reference_message : message_info->reference_messages) {
      if (reference_message->id == message_id) {
        return reference_message.get();
      }
    }
    return nullptr;
  }
  return get_message(message_info->chat_id, message_id, true);
}

td::int32 Client::get_forum_topic_id(const object_ptr<td_api::MessageTopic> &topic_id) {
  if (topic_id == nullptr) {
    return 0;
  }
  switch (topic_id->get_id()) {
    case td_api::messageTopicThread::ID:
      return as_client_message_id(static_cast<const td_api::messageTopicThread *>(topic_id.get())->message_thread_id_);
    case td_api::messageTopicForum::ID: {
      auto forum_topic_id = static_cast<const td_api::messageTopicForum *>(topic_id.get())->forum_topic_id_;
      if (forum_topic_id == GENERAL_FORUM_TOPIC_ID) {
        return 0;
      }
      return forum_topic_id;
    }
    case td_api::messageTopicDirectMessages::ID:
      return 0;
    case td_api::messageTopicSavedMessages::ID:
      return 0;
    default:
      UNREACHABLE();
      return 0;
  }
}

td::int64 Client::as_tdlib_message_id(int32 message_id) {
  return static_cast<int64>(message_id) << 20;
}

td::int32 Client::as_client_message_id(int64 message_id) {
  auto result = static_cast<int32>(message_id >> 20);
  CHECK(as_tdlib_message_id(result) == message_id);
  return result;
}

td::int32 Client::as_client_message_id_unchecked(int64 message_id) {
  auto result = static_cast<int32>(message_id >> 20);
  if (as_tdlib_message_id(result) != message_id) {
    return 0;
  }
  return result;
}

td::int64 Client::get_supergroup_chat_id(int64 supergroup_id) {
  return static_cast<int64>(-1000000000000ll) - supergroup_id;
}

td::int64 Client::get_basic_group_chat_id(int64 basic_group_id) {
  return -basic_group_id;
}

td::int64 Client::get_status_custom_emoji_id(const object_ptr<td_api::emojiStatus> &emoji_status) {
  if (emoji_status == nullptr) {
    return 0;
  }
  switch (emoji_status->type_->get_id()) {
    case td_api::emojiStatusTypeCustomEmoji::ID:
      return static_cast<const td_api::emojiStatusTypeCustomEmoji *>(emoji_status->type_.get())->custom_emoji_id_;
    case td_api::emojiStatusTypeUpgradedGift::ID:
      return static_cast<const td_api::emojiStatusTypeUpgradedGift *>(emoji_status->type_.get())
          ->model_custom_emoji_id_;
    default:
      UNREACHABLE();
      return 0;
  }
}

bool Client::is_topic_message(const object_ptr<td_api::MessageTopic> &topic_id) {
  return topic_id != nullptr && topic_id->get_id() == td_api::messageTopicForum::ID &&
         static_cast<const td_api::messageTopicForum *>(topic_id.get())->forum_topic_id_ != GENERAL_FORUM_TOPIC_ID;
}

td::FlatHashMap<td::string, td::Status (Client::*)(PromisedQueryPtr &query)> Client::methods_;

}  // namespace telegram_bot_api
