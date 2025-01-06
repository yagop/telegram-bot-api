# API Reference

**Note:** If you are looking for available [events](usage.md#events) or usage of api, please refer [`usage.md`](usage.md).

<a name="TelegramBot"></a>

## TelegramBot
TelegramBot

**Kind**: global class  
**See**: https://core.telegram.org/bots/api  

* [TelegramBot](#TelegramBot)
    * [new TelegramBot(token, [options])](#new_TelegramBot_new)
    * _instance_
        * [.on(event, listener)](#TelegramBot+on)
        * [.startPolling([options])](#TelegramBot+startPolling) ⇒ <code>Promise</code>
        * ~~[.initPolling([options])](#TelegramBot+initPolling) ⇒ <code>Promise</code>~~
        * [.stopPolling([options])](#TelegramBot+stopPolling) ⇒ <code>Promise</code>
        * [.getFileLink(fileId, [options])](#TelegramBot+getFileLink) ⇒ <code>Promise</code>
        * [.getFileStream(fileId, [options])](#TelegramBot+getFileStream) ⇒ <code>stream.Readable</code>
        * [.downloadFile(fileId, downloadDir, [options])](#TelegramBot+downloadFile) ⇒ <code>Promise</code>
        * [.onText(regexpRexecuted, callback)](#TelegramBot+onText)
        * [.removeTextListener(regexp)](#TelegramBot+removeTextListener) ⇒ <code>Object</code>
        * [.clearTextListeners()](#TelegramBot+clearTextListeners)
        * [.onReplyToMessage(chatId, messageId, callback)](#TelegramBot+onReplyToMessage) ⇒ <code>Number</code>
        * [.removeReplyListener(replyListenerId)](#TelegramBot+removeReplyListener) ⇒ <code>Object</code>
        * [.clearReplyListeners()](#TelegramBot+clearReplyListeners) ⇒ <code>Array</code>
        * [.isPolling()](#TelegramBot+isPolling) ⇒ <code>Boolean</code>
        * [.openWebHook()](#TelegramBot+openWebHook) ⇒ <code>Promise</code>
        * [.closeWebHook()](#TelegramBot+closeWebHook) ⇒ <code>Promise</code>
        * [.hasOpenWebHook()](#TelegramBot+hasOpenWebHook) ⇒ <code>Boolean</code>
        * [.processUpdate(update)](#TelegramBot+processUpdate)
        * [.getUpdates([options])](#TelegramBot+getUpdates) ⇒ <code>Promise</code>
        * [.setWebHook(url, [options], [fileOptions])](#TelegramBot+setWebHook) ⇒ <code>Promise</code>
        * [.deleteWebHook([options])](#TelegramBot+deleteWebHook) ⇒ <code>Promise</code>
        * [.getWebHookInfo([options])](#TelegramBot+getWebHookInfo) ⇒ <code>Promise</code>
        * [.getMe([options])](#TelegramBot+getMe) ⇒ <code>Promise</code>
        * [.logOut([options])](#TelegramBot+logOut) ⇒ <code>Promise</code>
        * [.close([options])](#TelegramBot+close) ⇒ <code>Promise</code>
        * [.sendMessage(chatId, text, [options])](#TelegramBot+sendMessage) ⇒ <code>Promise</code>
        * [.forwardMessage(chatId, fromChatId, messageId, [options])](#TelegramBot+forwardMessage) ⇒ <code>Promise</code>
        * [.forwardMessages(chatId, fromChatId, messageIds, [options])](#TelegramBot+forwardMessages) ⇒ <code>Promise</code>
        * [.copyMessage(chatId, fromChatId, messageId, [options])](#TelegramBot+copyMessage) ⇒ <code>Promise</code>
        * [.copyMessages(chatId, fromChatId, messageIds, [options])](#TelegramBot+copyMessages) ⇒ <code>Promise</code>
        * [.sendPhoto(chatId, photo, [options], [fileOptions])](#TelegramBot+sendPhoto) ⇒ <code>Promise</code>
        * [.sendAudio(chatId, audio, [options], [fileOptions])](#TelegramBot+sendAudio) ⇒ <code>Promise</code>
        * [.sendDocument(chatId, doc, [options], [fileOptions])](#TelegramBot+sendDocument) ⇒ <code>Promise</code>
        * [.sendVideo(chatId, video, [options], [fileOptions])](#TelegramBot+sendVideo) ⇒ <code>Promise</code>
        * [.sendAnimation(chatId, animation, [options], [fileOptions])](#TelegramBot+sendAnimation) ⇒ <code>Promise</code>
        * [.sendVoice(chatId, voice, [options], [fileOptions])](#TelegramBot+sendVoice) ⇒ <code>Promise</code>
        * [.sendVideoNote(chatId, videoNote, [options], [fileOptions])](#TelegramBot+sendVideoNote) ⇒ <code>Promise</code>
        * [.sendMediaGroup(chatId, media, [options])](#TelegramBot+sendMediaGroup) ⇒ <code>Promise</code>
        * [.sendLocation(chatId, latitude, longitude, [options])](#TelegramBot+sendLocation) ⇒ <code>Promise</code>
        * [.editMessageLiveLocation(latitude, longitude, [options])](#TelegramBot+editMessageLiveLocation) ⇒ <code>Promise</code>
        * [.stopMessageLiveLocation([options])](#TelegramBot+stopMessageLiveLocation) ⇒ <code>Promise</code>
        * [.sendVenue(chatId, latitude, longitude, title, address, [options])](#TelegramBot+sendVenue) ⇒ <code>Promise</code>
        * [.sendContact(chatId, phoneNumber, firstName, [options])](#TelegramBot+sendContact) ⇒ <code>Promise</code>
        * [.sendPoll(chatId, question, pollOptions, [options])](#TelegramBot+sendPoll) ⇒ <code>Promise</code>
        * [.sendDice(chatId, [options])](#TelegramBot+sendDice) ⇒ <code>Promise</code>
        * [.sendChatAction(chatId, action, [options])](#TelegramBot+sendChatAction) ⇒ <code>Promise</code>
        * [.setMessageReaction(chatId, messageId, [options])](#TelegramBot+setMessageReaction) ⇒ <code>[ &#x27;Promise&#x27; ].&lt;Boolean&gt;</code>
        * [.getUserProfilePhotos(userId, [options])](#TelegramBot+getUserProfilePhotos) ⇒ <code>Promise</code>
        * [.getFile(fileId, [options])](#TelegramBot+getFile) ⇒ <code>Promise</code>
        * [.banChatMember(chatId, userId, [options])](#TelegramBot+banChatMember) ⇒ <code>Promise</code>
        * [.unbanChatMember(chatId, userId, [options])](#TelegramBot+unbanChatMember) ⇒ <code>Promise</code>
        * [.restrictChatMember(chatId, userId, [options])](#TelegramBot+restrictChatMember) ⇒ <code>Promise</code>
        * [.promoteChatMember(chatId, userId, [options])](#TelegramBot+promoteChatMember) ⇒ <code>Promise</code>
        * [.setChatAdministratorCustomTitle(chatId, userId, customTitle, [options])](#TelegramBot+setChatAdministratorCustomTitle) ⇒ <code>Promise</code>
        * [.banChatSenderChat(chatId, senderChatId, [options])](#TelegramBot+banChatSenderChat) ⇒ <code>Promise</code>
        * [.unbanChatSenderChat(chatId, senderChatId, [options])](#TelegramBot+unbanChatSenderChat) ⇒ <code>Promise</code>
        * [.setChatPermissions(chatId, chatPermissions, [options])](#TelegramBot+setChatPermissions) ⇒ <code>Promise</code>
        * [.exportChatInviteLink(chatId, [options])](#TelegramBot+exportChatInviteLink) ⇒ <code>Promise</code>
        * [.createChatInviteLink(chatId, [options])](#TelegramBot+createChatInviteLink) ⇒ <code>Object</code>
        * [.editChatInviteLink(chatId, inviteLink, [options])](#TelegramBot+editChatInviteLink) ⇒ <code>Promise</code>
        * [.createChatSubscriptionInviteLink(chatId, subscriptionPeriod, subscriptionPrice, [options])](#TelegramBot+createChatSubscriptionInviteLink) ⇒ <code>Promise</code>
        * [.editChatSubscriptionInviteLink(chatId, inviteLink, [options])](#TelegramBot+editChatSubscriptionInviteLink) ⇒ <code>Promise</code>
        * [.revokeChatInviteLink(chatId, inviteLink, [options])](#TelegramBot+revokeChatInviteLink) ⇒ <code>Promise</code>
        * [.approveChatJoinRequest(chatId, userId, [options])](#TelegramBot+approveChatJoinRequest) ⇒ <code>Promise</code>
        * [.declineChatJoinRequest(chatId, userId, [options])](#TelegramBot+declineChatJoinRequest) ⇒ <code>Promise</code>
        * [.setChatPhoto(chatId, photo, [options], [fileOptions])](#TelegramBot+setChatPhoto) ⇒ <code>Promise</code>
        * [.deleteChatPhoto(chatId, [options])](#TelegramBot+deleteChatPhoto) ⇒ <code>Promise</code>
        * [.setChatTitle(chatId, title, [options])](#TelegramBot+setChatTitle) ⇒ <code>Promise</code>
        * [.setChatDescription(chatId, description, [options])](#TelegramBot+setChatDescription) ⇒ <code>Promise</code>
        * [.pinChatMessage(chatId, messageId, [options])](#TelegramBot+pinChatMessage) ⇒ <code>Promise</code>
        * [.unpinChatMessage(chatId, [options])](#TelegramBot+unpinChatMessage) ⇒ <code>Promise</code>
        * [.unpinAllChatMessages(chatId, [options])](#TelegramBot+unpinAllChatMessages) ⇒ <code>Promise</code>
        * [.leaveChat(chatId, [options])](#TelegramBot+leaveChat) ⇒ <code>Promise</code>
        * [.getChat(chatId, [options])](#TelegramBot+getChat) ⇒ <code>Promise</code>
        * [.getChatAdministrators(chatId, [options])](#TelegramBot+getChatAdministrators) ⇒ <code>Promise</code>
        * [.getChatMemberCount(chatId, [options])](#TelegramBot+getChatMemberCount) ⇒ <code>Promise</code>
        * [.getChatMember(chatId, userId, [options])](#TelegramBot+getChatMember) ⇒ <code>Promise</code>
        * [.setChatStickerSet(chatId, stickerSetName, [options])](#TelegramBot+setChatStickerSet) ⇒ <code>Promise</code>
        * [.deleteChatStickerSet(chatId, [options])](#TelegramBot+deleteChatStickerSet) ⇒ <code>Promise</code>
        * [.getForumTopicIconStickers(chatId, [options])](#TelegramBot+getForumTopicIconStickers) ⇒ <code>Promise</code>
        * [.createForumTopic(chatId, name, [options])](#TelegramBot+createForumTopic)
        * [.editForumTopic(chatId, messageThreadId, [options])](#TelegramBot+editForumTopic) ⇒ <code>Promise</code>
        * [.closeForumTopic(chatId, messageThreadId, [options])](#TelegramBot+closeForumTopic) ⇒ <code>Promise</code>
        * [.reopenForumTopic(chatId, messageThreadId, [options])](#TelegramBot+reopenForumTopic) ⇒ <code>Promise</code>
        * [.deleteForumTopic(chatId, messageThreadId, [options])](#TelegramBot+deleteForumTopic) ⇒ <code>Promise</code>
        * [.unpinAllForumTopicMessages(chatId, messageThreadId, [options])](#TelegramBot+unpinAllForumTopicMessages) ⇒ <code>Promise</code>
        * [.editGeneralForumTopic(chatId, name, [options])](#TelegramBot+editGeneralForumTopic) ⇒ <code>Promise</code>
        * [.closeGeneralForumTopic(chatId, [options])](#TelegramBot+closeGeneralForumTopic) ⇒ <code>Promise</code>
        * [.reopenGeneralForumTopic(chatId, [options])](#TelegramBot+reopenGeneralForumTopic) ⇒ <code>Promise</code>
        * [.hideGeneralForumTopic(chatId, [options])](#TelegramBot+hideGeneralForumTopic) ⇒ <code>Promise</code>
        * [.unhideGeneralForumTopic(chatId, [options])](#TelegramBot+unhideGeneralForumTopic) ⇒ <code>Promise</code>
        * [.unpinAllGeneralForumTopicMessages(chatId, [options])](#TelegramBot+unpinAllGeneralForumTopicMessages) ⇒ <code>Promise</code>
        * [.answerCallbackQuery(callbackQueryId, [options])](#TelegramBot+answerCallbackQuery) ⇒ <code>Promise</code>
        * [.savePreparedInlineMessage(userId, result, [form])](#TelegramBot+savePreparedInlineMessage) ⇒ <code>Promise</code>
        * [.getUserChatBoosts(chatId, userId, [options])](#TelegramBot+getUserChatBoosts) ⇒ <code>Promise</code>
        * [.getBusinessConnection(businessConnectionId, [options])](#TelegramBot+getBusinessConnection) ⇒ <code>Promise</code>
        * [.setMyCommands(commands, [options])](#TelegramBot+setMyCommands) ⇒ <code>Promise</code>
        * [.deleteMyCommands([options])](#TelegramBot+deleteMyCommands) ⇒ <code>Promise</code>
        * [.getMyCommands([options])](#TelegramBot+getMyCommands) ⇒ <code>Promise</code>
        * [.setMyName([options])](#TelegramBot+setMyName) ⇒ <code>Promise</code>
        * [.getMyName([options])](#TelegramBot+getMyName) ⇒ <code>Promise</code>
        * [.setMyDescription([options])](#TelegramBot+setMyDescription) ⇒ <code>Promise</code>
        * [.getMyDescription([options])](#TelegramBot+getMyDescription) ⇒ <code>Promise</code>
        * [.setMyShortDescription([options])](#TelegramBot+setMyShortDescription) ⇒ <code>Promise</code>
        * [.getMyShortDescription([options])](#TelegramBot+getMyShortDescription) ⇒ <code>Promise</code>
        * [.setChatMenuButton([options])](#TelegramBot+setChatMenuButton) ⇒ <code>Promise</code>
        * [.getChatMenuButton([options])](#TelegramBot+getChatMenuButton) ⇒ <code>Promise</code>
        * [.setMyDefaultAdministratorRights([options])](#TelegramBot+setMyDefaultAdministratorRights) ⇒ <code>Promise</code>
        * [.getMyDefaultAdministratorRights([options])](#TelegramBot+getMyDefaultAdministratorRights) ⇒ <code>Promise</code>
        * [.editMessageText(text, [options])](#TelegramBot+editMessageText) ⇒ <code>Promise</code>
        * [.editMessageCaption(caption, [options])](#TelegramBot+editMessageCaption) ⇒ <code>Promise</code>
        * [.editMessageMedia(media, [options])](#TelegramBot+editMessageMedia) ⇒ <code>Promise</code>
        * [.editMessageReplyMarkup(replyMarkup, [options])](#TelegramBot+editMessageReplyMarkup) ⇒ <code>Promise</code>
        * [.stopPoll(chatId, pollId, [options])](#TelegramBot+stopPoll) ⇒ <code>Promise</code>
        * [.sendSticker(chatId, sticker, [options], [fileOptions])](#TelegramBot+sendSticker) ⇒ <code>Promise</code>
        * [.getStickerSet(name, [options])](#TelegramBot+getStickerSet) ⇒ <code>Promise</code>
        * [.getCustomEmojiStickers(custom_emoji_ids, [options])](#TelegramBot+getCustomEmojiStickers) ⇒ <code>Promise</code>
        * [.uploadStickerFile(userId, sticker, stickerFormat, [options], [fileOptions])](#TelegramBot+uploadStickerFile) ⇒ <code>Promise</code>
        * [.createNewStickerSet(userId, name, title, pngSticker, emojis, [options], [fileOptions])](#TelegramBot+createNewStickerSet) ⇒ <code>Promise</code>
        * [.addStickerToSet(userId, name, sticker, emojis, stickerType, [options], [fileOptions])](#TelegramBot+addStickerToSet) ⇒ <code>Promise</code>
        * [.setStickerPositionInSet(sticker, position, [options])](#TelegramBot+setStickerPositionInSet) ⇒ <code>Promise</code>
        * [.deleteStickerFromSet(sticker, [options])](#TelegramBot+deleteStickerFromSet) ⇒ <code>Promise</code>
        * [.replaceStickerInSet(user_id, name, sticker, [options])](#TelegramBot+replaceStickerInSet) ⇒ <code>Promise</code>
        * [.setStickerEmojiList(sticker, emojiList, [options])](#TelegramBot+setStickerEmojiList) ⇒ <code>Promise</code>
        * [.setStickerKeywords(sticker, [options])](#TelegramBot+setStickerKeywords) ⇒ <code>Promise</code>
        * [.setStickerMaskPosition(sticker, [options])](#TelegramBot+setStickerMaskPosition) ⇒ <code>Promise</code>
        * [.setStickerSetTitle(name, title, [options])](#TelegramBot+setStickerSetTitle) ⇒ <code>Promise</code>
        * [.setStickerSetThumbnail(userId, name, thumbnail, [options], [fileOptions])](#TelegramBot+setStickerSetThumbnail) ⇒ <code>Promise</code>
        * [.setCustomEmojiStickerSetThumbnail(name, [options])](#TelegramBot+setCustomEmojiStickerSetThumbnail) ⇒ <code>Promise</code>
        * [.deleteStickerSet(name, [options])](#TelegramBot+deleteStickerSet) ⇒ <code>Promise</code>
        * [.answerInlineQuery(inlineQueryId, results, [options])](#TelegramBot+answerInlineQuery) ⇒ <code>Promise</code>
        * [.answerWebAppQuery(webAppQueryId, result, [options])](#TelegramBot+answerWebAppQuery) ⇒ <code>Promise</code>
        * [.sendInvoice(chatId, title, description, payload, providerToken, currency, prices, [options])](#TelegramBot+sendInvoice) ⇒ <code>Promise</code>
        * [.createInvoiceLink(title, description, payload, providerToken, currency, prices, [options])](#TelegramBot+createInvoiceLink) ⇒ <code>Promise</code>
        * [.answerShippingQuery(shippingQueryId, ok, [options])](#TelegramBot+answerShippingQuery) ⇒ <code>Promise</code>
        * [.answerPreCheckoutQuery(preCheckoutQueryId, ok, [options])](#TelegramBot+answerPreCheckoutQuery) ⇒ <code>Promise</code>
        * [.getStarTransactions([options])](#TelegramBot+getStarTransactions) ⇒ <code>Promise</code>
        * [.refundStarPayment(userId, telegramPaymentChargeId, [options])](#TelegramBot+refundStarPayment) ⇒ <code>Promise</code>
        * [.sendGame(chatId, gameShortName, [options])](#TelegramBot+sendGame) ⇒ <code>Promise</code>
        * [.setGameScore(userId, score, [options])](#TelegramBot+setGameScore) ⇒ <code>Promise</code>
        * [.getGameHighScores(userId, [options])](#TelegramBot+getGameHighScores) ⇒ <code>Promise</code>
        * [.deleteMessage(chatId, messageId, [options])](#TelegramBot+deleteMessage) ⇒ <code>Promise</code>
        * [.deleteMessages(chatId, messageIds, [options])](#TelegramBot+deleteMessages) ⇒ <code>[ &#x27;Promise&#x27; ].&lt;Boolean&gt;</code>
    * _static_
        * [.errors](#TelegramBot.errors) : <code>Object</code>
        * [.messageTypes](#TelegramBot.messageTypes) : <code>[ &#x27;Array&#x27; ].&lt;String&gt;</code>

<a name="new_TelegramBot_new"></a>

### new TelegramBot(token, [options])
Both request method to obtain messages are implemented. To use standard polling, set `polling: true`on `options`. Notice that [webHook](https://core.telegram.org/bots/api#setwebhook) will need a SSL certificate.Emits `message` when a message arrives.


| Param | Type | Default | Description |
| --- | --- | --- | --- |
| token | <code>String</code> |  | Bot Token |
| [options] | <code>Object</code> |  |  |
| [options.polling] | <code>Boolean</code> \| <code>Object</code> | <code>false</code> | Set true to enable polling or set options.  If a WebHook has been set, it will be deleted automatically. |
| [options.polling.timeout] | <code>String</code> \| <code>Number</code> | <code>10</code> | *Deprecated. Use `options.polling.params` instead*.  Timeout in seconds for long polling. |
| [options.testEnvironment] | <code>Boolean</code> | <code>false</code> | Set true to  work with test enviroment. When working with the test environment, you may use HTTP links without TLS to test your Web App. |
| [options.polling.interval] | <code>String</code> \| <code>Number</code> | <code>300</code> | Interval between requests in miliseconds |
| [options.polling.autoStart] | <code>Boolean</code> | <code>true</code> | Start polling immediately |
| [options.polling.params] | <code>Object</code> |  | Parameters to be used in polling API requests.  See https://core.telegram.org/bots/api#getupdates for more information. |
| [options.polling.params.timeout] | <code>Number</code> | <code>10</code> | Timeout in seconds for long polling. |
| [options.webHook] | <code>Boolean</code> \| <code>Object</code> | <code>false</code> | Set true to enable WebHook or set options |
| [options.webHook.host] | <code>String</code> | <code>&quot;0.0.0.0&quot;</code> | Host to bind to |
| [options.webHook.port] | <code>Number</code> | <code>8443</code> | Port to bind to |
| [options.webHook.key] | <code>String</code> |  | Path to file with PEM private key for webHook server.  The file is read **synchronously**! |
| [options.webHook.cert] | <code>String</code> |  | Path to file with PEM certificate (public) for webHook server.  The file is read **synchronously**! |
| [options.webHook.pfx] | <code>String</code> |  | Path to file with PFX private key and certificate chain for webHook server.  The file is read **synchronously**! |
| [options.webHook.autoOpen] | <code>Boolean</code> | <code>true</code> | Open webHook immediately |
| [options.webHook.https] | <code>Object</code> |  | Options to be passed to `https.createServer()`.  Note that `options.webHook.key`, `options.webHook.cert` and `options.webHook.pfx`, if provided, will be  used to override `key`, `cert` and `pfx` in this object, respectively.  See https://nodejs.org/api/https.html#https_https_createserver_options_requestlistener for more information. |
| [options.webHook.healthEndpoint] | <code>String</code> | <code>&quot;/healthz&quot;</code> | An endpoint for health checks that always responds with 200 OK |
| [options.onlyFirstMatch] | <code>Boolean</code> | <code>false</code> | Set to true to stop after first match. Otherwise, all regexps are executed |
| [options.request] | <code>Object</code> |  | Options which will be added for all requests to telegram api.  See https://github.com/request/request#requestoptions-callback for more information. |
| [options.baseApiUrl] | <code>String</code> | <code>&quot;https://api.telegram.org&quot;</code> | API Base URl; useful for proxying and testing |
| [options.filepath] | <code>Boolean</code> | <code>true</code> | Allow passing file-paths as arguments when sending files,  such as photos using `TelegramBot#sendPhoto()`. See [usage information][usage-sending-files-performance]  for more information on this option and its consequences. |
| [options.badRejection] | <code>Boolean</code> | <code>false</code> | Set to `true`  **if and only if** the Node.js version you're using terminates the  process on unhandled rejections. This option is only for  *forward-compatibility purposes*. |

<a name="TelegramBot+on"></a>

### telegramBot.on(event, listener)
Add listener for the specified [event](https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#events).This is the usual `emitter.on()` method.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**See**

- [Available events](https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#events)
- https://nodejs.org/api/events.html#events_emitter_on_eventname_listener


| Param | Type |
| --- | --- |
| event | <code>String</code> | 
| listener | <code>function</code> | 

<a name="TelegramBot+startPolling"></a>

### telegramBot.startPolling([options]) ⇒ <code>Promise</code>
Start polling.Rejects returned promise if a WebHook is being used by this instance.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  

| Param | Type | Default | Description |
| --- | --- | --- | --- |
| [options] | <code>Object</code> |  |  |
| [options.restart] | <code>Boolean</code> | <code>true</code> | Consecutive calls to this method causes polling to be restarted |

<a name="TelegramBot+initPolling"></a>

### ~~telegramBot.initPolling([options]) ⇒ <code>Promise</code>~~
***Deprecated***

Alias of `TelegramBot#startPolling()`. This is **deprecated**.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  

| Param | Type |
| --- | --- |
| [options] | <code>Object</code> | 

<a name="TelegramBot+stopPolling"></a>

### telegramBot.stopPolling([options]) ⇒ <code>Promise</code>
Stops polling after the last polling request resolves.Multiple invocations do nothing if polling is already stopped.Returning the promise of the last polling request is **deprecated**.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Options |
| [options.cancel] | <code>Boolean</code> | Cancel current request |
| [options.reason] | <code>String</code> | Reason for stopping polling |

<a name="TelegramBot+getFileLink"></a>

### telegramBot.getFileLink(fileId, [options]) ⇒ <code>Promise</code>
Get link for file.Use this method to get link for file for subsequent use.Attention: link will be valid for 1 hour.This method is a sugar extension of the (getFile)[#getfilefileid] method,which returns just path to file on remote server (you will have to manually build full uri after that).

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - Promise which will have  *fileURI* in resolve callback  
**See**: https://core.telegram.org/bots/api#getfile  

| Param | Type | Description |
| --- | --- | --- |
| fileId | <code>String</code> | File identifier to get info about |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getFileStream"></a>

### telegramBot.getFileStream(fileId, [options]) ⇒ <code>stream.Readable</code>
Return a readable stream for file.`fileStream.path` is the specified file ID i.e. `fileId`.`fileStream` emits event `info` passing a single argument i.e.`info` with the interface `{ uri }` where `uri` is the URI of thefile on Telegram servers.This method is a sugar extension of the [getFileLink](#TelegramBot+getFileLink) method,which returns the full URI to the file on remote server.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>stream.Readable</code> - fileStream  

| Param | Type | Description |
| --- | --- | --- |
| fileId | <code>String</code> | File identifier to get info about |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+downloadFile"></a>

### telegramBot.downloadFile(fileId, downloadDir, [options]) ⇒ <code>Promise</code>
Downloads file in the specified folder.This method is a sugar extension of the [getFileStream](#TelegramBot+getFileStream) method,which returns a readable file stream.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - Promise, which will have *filePath* of downloaded file in resolve callback  

| Param | Type | Description |
| --- | --- | --- |
| fileId | <code>String</code> | File identifier to get info about |
| downloadDir | <code>String</code> | Absolute path to the folder in which file will be saved |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+onText"></a>

### telegramBot.onText(regexpRexecuted, callback)
Register a RegExp to test against an incomming text message.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  

| Param | Type | Description |
| --- | --- | --- |
| regexpRexecuted | <code>RegExp</code> | with `exec`. |
| callback | <code>function</code> | Callback will be called with 2 parameters, the `msg` and the result of executing `regexp.exec` on message text. |

<a name="TelegramBot+removeTextListener"></a>

### telegramBot.removeTextListener(regexp) ⇒ <code>Object</code>
Remove a listener registered with `onText()`.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Object</code> - deletedListener The removed reply listener if  found. This object has `regexp` and `callback`  properties. If not found, returns `null`.  

| Param | Type | Description |
| --- | --- | --- |
| regexp | <code>RegExp</code> | RegExp used previously in `onText()` |

<a name="TelegramBot+clearTextListeners"></a>

### telegramBot.clearTextListeners()
Remove all listeners registered with `onText()`.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
<a name="TelegramBot+onReplyToMessage"></a>

### telegramBot.onReplyToMessage(chatId, messageId, callback) ⇒ <code>Number</code>
Register a reply to wait for a message response.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Number</code> - id The ID of the inserted reply listener.  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | The chat id where the message cames from. |
| messageId | <code>Number</code> \| <code>String</code> | The message id to be replied. |
| callback | <code>function</code> | Callback will be called with the reply  message. |

<a name="TelegramBot+removeReplyListener"></a>

### telegramBot.removeReplyListener(replyListenerId) ⇒ <code>Object</code>
Removes a reply that has been prev. registered for a message response.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Object</code> - deletedListener The removed reply listener if  found. This object has `id`, `chatId`, `messageId` and `callback`  properties. If not found, returns `null`.  

| Param | Type | Description |
| --- | --- | --- |
| replyListenerId | <code>Number</code> | The ID of the reply listener. |

<a name="TelegramBot+clearReplyListeners"></a>

### telegramBot.clearReplyListeners() ⇒ <code>Array</code>
Removes all replies that have been prev. registered for a message response.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Array</code> - deletedListeners An array of removed listeners.  
<a name="TelegramBot+isPolling"></a>

### telegramBot.isPolling() ⇒ <code>Boolean</code>
Return true if polling. Otherwise, false.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
<a name="TelegramBot+openWebHook"></a>

### telegramBot.openWebHook() ⇒ <code>Promise</code>
Open webhook.Multiple invocations do nothing if webhook is already open.Rejects returned promise if Polling is being used by this instance.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
<a name="TelegramBot+closeWebHook"></a>

### telegramBot.closeWebHook() ⇒ <code>Promise</code>
Close webhook after closing all current connections.Multiple invocations do nothing if webhook is already closed.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - Promise  
<a name="TelegramBot+hasOpenWebHook"></a>

### telegramBot.hasOpenWebHook() ⇒ <code>Boolean</code>
Return true if using webhook and it is open i.e. accepts connections.Otherwise, false.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
<a name="TelegramBot+processUpdate"></a>

### telegramBot.processUpdate(update)
Process an update; emitting the proper events and executing regexpcallbacks. This method is useful should you be using a differentway to fetch updates, other than those provided by TelegramBot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**See**: https://core.telegram.org/bots/api#update  

| Param | Type |
| --- | --- |
| update | <code>Object</code> | 

<a name="TelegramBot+getUpdates"></a>

### telegramBot.getUpdates([options]) ⇒ <code>Promise</code>
Use this method to receive incoming updates using long polling.This method has an [older, compatible signature][getUpdates-v0.25.0]that is being deprecated.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**See**: https://core.telegram.org/bots/api#getupdates  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setWebHook"></a>

### telegramBot.setWebHook(url, [options], [fileOptions]) ⇒ <code>Promise</code>
Specify an url to receive incoming updates via an outgoing webHook.This method has an [older, compatible signature][setWebHook-v0.25.0]that is being deprecated.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**See**

- https://core.telegram.org/bots/api#setwebhook
- https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files


| Param | Type | Description |
| --- | --- | --- |
| url | <code>String</code> | URL where Telegram will make HTTP Post. Leave empty to delete webHook. |
| [options] | <code>Object</code> | Additional Telegram query options |
| [options.certificate] | <code>String</code> \| <code>stream.Stream</code> | PEM certificate key (public). |
| [options.secret_token] | <code>String</code> | Optional secret token to be sent in a header `X-Telegram-Bot-Api-Secret-Token` in every webhook request. |
| [fileOptions] | <code>Object</code> | Optional file related meta-data |

<a name="TelegramBot+deleteWebHook"></a>

### telegramBot.deleteWebHook([options]) ⇒ <code>Promise</code>
Use this method to remove webhook integration if you decide toswitch back to getUpdates. Returns True on success.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**See**: https://core.telegram.org/bots/api#deletewebhook  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getWebHookInfo"></a>

### telegramBot.getWebHookInfo([options]) ⇒ <code>Promise</code>
Use this method to get current webhook status.On success, returns a [WebhookInfo](https://core.telegram.org/bots/api#webhookinfo) object.If the bot is using getUpdates, will return an object with theurl field empty.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**See**: https://core.telegram.org/bots/api#getwebhookinfo  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getMe"></a>

### telegramBot.getMe([options]) ⇒ <code>Promise</code>
A simple method for testing your bot's authentication token. Requires no parameters.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - basic information about the bot in form of a [User](https://core.telegram.org/bots/api#user) object.  
**See**: https://core.telegram.org/bots/api#getme  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+logOut"></a>

### telegramBot.logOut([options]) ⇒ <code>Promise</code>
This method log out your bot from the cloud Bot API server before launching the bot locally.You must log out the bot before running it locally, otherwise there is no guarantee that the bot will receive updates.After a successful call, you will not be able to log in again using the same token for 10 minutes.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#logout  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+close"></a>

### telegramBot.close([options]) ⇒ <code>Promise</code>
This method close the bot instance before moving it from one local server to another.This method will return error 429 in the first 10 minutes after the bot is launched.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#close  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+sendMessage"></a>

### telegramBot.sendMessage(chatId, text, [options]) ⇒ <code>Promise</code>
Send text message.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned  
**See**: https://core.telegram.org/bots/api#sendmessage  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| text | <code>String</code> | Text of the message to be sent |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+forwardMessage"></a>

### telegramBot.forwardMessage(chatId, fromChatId, messageId, [options]) ⇒ <code>Promise</code>
Forward messages of any kind.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**See**: https://core.telegram.org/bots/api#forwardmessage  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) or username of the target channel (in the format `@channelusername`) |
| fromChatId | <code>Number</code> \| <code>String</code> | Unique identifier for the chat where the original message was sent (or channel username in the format `@channelusername`) |
| messageId | <code>Number</code> \| <code>String</code> | Unique message identifier in the chat specified in fromChatId |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+forwardMessages"></a>

### telegramBot.forwardMessages(chatId, fromChatId, messageIds, [options]) ⇒ <code>Promise</code>
Use this method to forward multiple messages of any kind.If some of the specified messages can't be found or forwarded, they are skipped.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - An array of MessageId of the sent messages on success  
**See**: https://core.telegram.org/bots/api#forwardmessages  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) or username of the target channel (in the format `@channelusername`) |
| fromChatId | <code>Number</code> \| <code>String</code> | Unique identifier for the chat where the original message was sent (or channel username in the format `@channelusername`) |
| messageIds | <code>[ &#x27;Array&#x27; ].&lt;(Number\|String)&gt;</code> | Identifiers of 1-100 messages in the chat from_chat_id to forward. The identifiers must be specified in a strictly increasing order. |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+copyMessage"></a>

### telegramBot.copyMessage(chatId, fromChatId, messageId, [options]) ⇒ <code>Promise</code>
Copy messages of any kind. **Service messages and invoice messages can't be copied.**The method is analogous to the method forwardMessages, but the copied message doesn'thave a link to the original message.Returns the MessageId of the sent message on success.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - The [MessageId](https://core.telegram.org/bots/api#messageid) of the sent message on success  
**See**: https://core.telegram.org/bots/api#copymessage  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| fromChatId | <code>Number</code> \| <code>String</code> | Unique identifier for the chat where the original message was sent |
| messageId | <code>Number</code> \| <code>String</code> | Unique message identifier |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+copyMessages"></a>

### telegramBot.copyMessages(chatId, fromChatId, messageIds, [options]) ⇒ <code>Promise</code>
Use this method to copy messages of any kind. If some of the specified messages can't be found or copied, they are skipped.Service messages, giveaway messages, giveaway winners messages, and invoice messages can't be copied.Returns the MessageId of the sent message on success.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - An array of MessageId of the sent messages  
**See**: https://core.telegram.org/bots/api#copymessages  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat |
| fromChatId | <code>Number</code> \| <code>String</code> | Unique identifier for the chat where the original message was sent |
| messageIds | <code>Array</code> | Identifiers of 1-100 messages in the chat from_chat_id to copy. The identifiers must be specified in a strictly increasing order. |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+sendPhoto"></a>

### telegramBot.sendPhoto(chatId, photo, [options], [fileOptions]) ⇒ <code>Promise</code>
Send photo

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned  
**See**

- https://core.telegram.org/bots/api#sendphoto
- https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files


| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| photo | <code>String</code> \| <code>stream.Stream</code> \| <code>Buffer</code> | A file path or a Stream. Can also be a `file_id` previously uploaded |
| [options] | <code>Object</code> | Additional Telegram query options |
| [fileOptions] | <code>Object</code> | Optional file related meta-data |

<a name="TelegramBot+sendAudio"></a>

### telegramBot.sendAudio(chatId, audio, [options], [fileOptions]) ⇒ <code>Promise</code>
Send audio**Your audio must be in the .MP3 or .M4A format.**

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned  
**See**

- https://core.telegram.org/bots/api#sendaudio
- https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files


| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| audio | <code>String</code> \| <code>stream.Stream</code> \| <code>Buffer</code> | A file path, Stream or Buffer. Can also be a `file_id` previously uploaded. |
| [options] | <code>Object</code> | Additional Telegram query options |
| [fileOptions] | <code>Object</code> | Optional file related meta-data |

<a name="TelegramBot+sendDocument"></a>

### telegramBot.sendDocument(chatId, doc, [options], [fileOptions]) ⇒ <code>Promise</code>
Send Document

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned  
**See**

- https://core.telegram.org/bots/api#sendDocument
- https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files


| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| doc | <code>String</code> \| <code>stream.Stream</code> \| <code>Buffer</code> | A file path, Stream or Buffer. Can also be a `file_id` previously uploaded. |
| [options] | <code>Object</code> | Additional Telegram query options |
| [fileOptions] | <code>Object</code> | Optional file related meta-data |

<a name="TelegramBot+sendVideo"></a>

### telegramBot.sendVideo(chatId, video, [options], [fileOptions]) ⇒ <code>Promise</code>
Use this method to send video files, **Telegram clients support mp4 videos** (other formats may be sent as Document).

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned  
**See**

- https://core.telegram.org/bots/api#sendvideo
- https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files


| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| video | <code>String</code> \| <code>stream.Stream</code> \| <code>Buffer</code> | A file path or Stream. Can also be a `file_id` previously uploaded. |
| [options] | <code>Object</code> | Additional Telegram query options |
| [fileOptions] | <code>Object</code> | Optional file related meta-data |

<a name="TelegramBot+sendAnimation"></a>

### telegramBot.sendAnimation(chatId, animation, [options], [fileOptions]) ⇒ <code>Promise</code>
Use this method to send animation files (GIF or H.264/MPEG-4 AVC video without sound).

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned  
**See**

- https://core.telegram.org/bots/api#sendanimation
- https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files


| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| animation | <code>String</code> \| <code>stream.Stream</code> \| <code>Buffer</code> | A file path, Stream or Buffer. Can also be a `file_id` previously uploaded. |
| [options] | <code>Object</code> | Additional Telegram query options |
| [fileOptions] | <code>Object</code> | Optional file related meta-data |

<a name="TelegramBot+sendVoice"></a>

### telegramBot.sendVoice(chatId, voice, [options], [fileOptions]) ⇒ <code>Promise</code>
Send voice**Your audio must be in an .OGG file encoded with OPUS**, or in .MP3 format, or in .M4A format (other formats may be sent as Audio or Document)

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned  
**See**

- https://core.telegram.org/bots/api#sendvoice
- https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files


| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| voice | <code>String</code> \| <code>stream.Stream</code> \| <code>Buffer</code> | A file path, Stream or Buffer. Can also be a `file_id` previously uploaded. |
| [options] | <code>Object</code> | Additional Telegram query options |
| [fileOptions] | <code>Object</code> | Optional file related meta-data |

<a name="TelegramBot+sendVideoNote"></a>

### telegramBot.sendVideoNote(chatId, videoNote, [options], [fileOptions]) ⇒ <code>Promise</code>
Use this method to send video messagesTelegram clients support **rounded square MPEG4 videos** of up to 1 minute long.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned  
**Info**: The length parameter is actually optional. However, the API (at time of writing) requires you to always provide it until it is fixed.  
**See**

- https://core.telegram.org/bots/api#sendvideonote
- https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files


| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| videoNote | <code>String</code> \| <code>stream.Stream</code> \| <code>Buffer</code> | A file path or Stream. Can also be a `file_id` previously uploaded. |
| [options] | <code>Object</code> | Additional Telegram query options |
| [fileOptions] | <code>Object</code> | Optional file related meta-data |

<a name="TelegramBot+sendMediaGroup"></a>

### telegramBot.sendMediaGroup(chatId, media, [options]) ⇒ <code>Promise</code>
Use this method to send a group of photos or videos as an album.**Documents and audio files can be only grouped in an album with messages of the same type**If you wish to [specify file options](https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files),add a `fileOptions` property to the target input in `media`.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, an array of the sent [Messages](https://core.telegram.org/bots/api#message)is returned.  
**See**

- https://core.telegram.org/bots/api#sendmediagroup
- https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files


| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| media | <code>Array</code> | A JSON-serialized array describing photos and videos to be sent, must include 2–10 items |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+sendLocation"></a>

### telegramBot.sendLocation(chatId, latitude, longitude, [options]) ⇒ <code>Promise</code>
Send location.Use this method to send point on the map.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned  
**See**: https://core.telegram.org/bots/api#sendlocation  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| latitude | <code>Float</code> | Latitude of location |
| longitude | <code>Float</code> | Longitude of location |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+editMessageLiveLocation"></a>

### telegramBot.editMessageLiveLocation(latitude, longitude, [options]) ⇒ <code>Promise</code>
Use this method to edit live location messages sent bythe bot or via the bot (for inline bots). A location **can be edited until its live_period expires or editing is explicitly disabled by a call to [stopMessageLiveLocation](https://core.telegram.org/bots/api#stopmessagelivelocation)**Note that you must provide one of chat_id, message_id, orinline_message_id in your request.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, if the edited message is not an inline message, the edited [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned.  
**See**: https://core.telegram.org/bots/api#editmessagelivelocation  

| Param | Type | Description |
| --- | --- | --- |
| latitude | <code>Float</code> | Latitude of location |
| longitude | <code>Float</code> | Longitude of location |
| [options] | <code>Object</code> | Additional Telegram query options (provide either one of chat_id, message_id, or inline_message_id here) |

<a name="TelegramBot+stopMessageLiveLocation"></a>

### telegramBot.stopMessageLiveLocation([options]) ⇒ <code>Promise</code>
Use this method to stop updating a live location message sent bythe bot or via the bot (for inline bots) before live_period expires.Note that you must provide one of chat_id, message_id, orinline_message_id in your request.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, if the edited message is not an inline message, the edited [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned.  
**See**: https://core.telegram.org/bots/api#stopmessagelivelocation  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options (provide either one of chat_id, message_id, or inline_message_id here) |

<a name="TelegramBot+sendVenue"></a>

### telegramBot.sendVenue(chatId, latitude, longitude, title, address, [options]) ⇒ <code>Promise</code>
Send venue.Use this method to send information about a venue.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned.  
**See**: https://core.telegram.org/bots/api#sendvenue  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| latitude | <code>Float</code> | Latitude of location |
| longitude | <code>Float</code> | Longitude of location |
| title | <code>String</code> | Name of the venue |
| address | <code>String</code> | Address of the venue |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+sendContact"></a>

### telegramBot.sendContact(chatId, phoneNumber, firstName, [options]) ⇒ <code>Promise</code>
Send contact.Use this method to send phone contacts.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned  
**See**: https://core.telegram.org/bots/api#sendcontact  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| phoneNumber | <code>String</code> | Contact's phone number |
| firstName | <code>String</code> | Contact's first name |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+sendPoll"></a>

### telegramBot.sendPoll(chatId, question, pollOptions, [options]) ⇒ <code>Promise</code>
Send poll.Use this method to send a native poll.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned  
**See**: https://core.telegram.org/bots/api#sendpoll  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the group/channel |
| question | <code>String</code> | Poll question, 1-300 characters |
| pollOptions | <code>Array</code> | Poll options, between 2-10 options (only 1-100 characters each) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+sendDice"></a>

### telegramBot.sendDice(chatId, [options]) ⇒ <code>Promise</code>
Send DiceUse this method to send an animated emoji that will display a random value.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned  
**See**: https://core.telegram.org/bots/api#senddice  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+sendChatAction"></a>

### telegramBot.sendChatAction(chatId, action, [options]) ⇒ <code>Promise</code>
Send chat action.Use this method when you need to tell the user that something is happening on the bot's side.**The status is set for 5 seconds or less** (when a message arrives from your bot, Telegram clients clear its typing status). Action `typing` for [text messages](https://core.telegram.org/bots/api#sendmessage),`upload_photo` for [photos](https://core.telegram.org/bots/api#sendphoto), `record_video` or `upload_video` for [videos](https://core.telegram.org/bots/api#sendvideo),`record_voice` or `upload_voice` for [voice notes](https://core.telegram.org/bots/api#sendvoice), `upload_document` for [general files](https://core.telegram.org/bots/api#senddocument),`choose_sticker` for [stickers](https://core.telegram.org/bots/api#sendsticker), `find_location` for [location data](https://core.telegram.org/bots/api#sendlocation),`record_video_note` or `upload_video_note` for [video notes](https://core.telegram.org/bots/api#sendvideonote).

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#sendchataction  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| action | <code>String</code> | Type of action to broadcast. |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setMessageReaction"></a>

### telegramBot.setMessageReaction(chatId, messageId, [options]) ⇒ <code>[ &#x27;Promise&#x27; ].&lt;Boolean&gt;</code>
Use this method to change the chosen reactions on a message.- Service messages can't be reacted to.- Automatically forwarded messages from a channel to its discussion group have the same available reactions as messages in the channel.- In albums, bots must react to the first message.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>[ &#x27;Promise&#x27; ].&lt;Boolean&gt;</code> - True on success  
**See**: https://core.telegram.org/bots/api#setmessagereaction  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format @channelusername) |
| messageId | <code>Number</code> | Unique identifier of the target message |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getUserProfilePhotos"></a>

### telegramBot.getUserProfilePhotos(userId, [options]) ⇒ <code>Promise</code>
Use this method to get a list of profile pictures for a user.Returns a [UserProfilePhotos](https://core.telegram.org/bots/api#userprofilephotos) object.This method has an [older, compatible signature][getUserProfilePhotos-v0.25.0]that is being deprecated.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - Returns a [UserProfilePhotos](https://core.telegram.org/bots/api#userprofilephotos) object  
**See**: https://core.telegram.org/bots/api#getuserprofilephotos  

| Param | Type | Description |
| --- | --- | --- |
| userId | <code>Number</code> | Unique identifier of the target user |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getFile"></a>

### telegramBot.getFile(fileId, [options]) ⇒ <code>Promise</code>
Get file.Use this method to get basic info about a file and prepare it for downloading.Attention: **link will be valid for 1 hour.**

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, a [File](https://core.telegram.org/bots/api#file) object is returned  
**See**: https://core.telegram.org/bots/api#getfile  

| Param | Type | Description |
| --- | --- | --- |
| fileId | <code>String</code> | File identifier to get info about |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+banChatMember"></a>

### telegramBot.banChatMember(chatId, userId, [options]) ⇒ <code>Promise</code>
Use this method to ban a user in a group, a supergroup or a channel.In the case of supergroups and channels, the user will not be able toreturn to the chat on their own using invite links, etc., unless unbanned first..The **bot must be an administrator in the group, supergroup or a channel** for this to work.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success.  
**See**: https://core.telegram.org/bots/api#banchatmember  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| userId | <code>Number</code> | Unique identifier of the target user |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+unbanChatMember"></a>

### telegramBot.unbanChatMember(chatId, userId, [options]) ⇒ <code>Promise</code>
Use this method to unban a previously kicked user in a supergroup.The user will not return to the group automatically, but will beable to join via link, etc.The **bot must be an administrator** in the supergroup or channel for this to work.**By default**, this method guarantees that after the call the user is not a member of the chat, but will be able to join it.So **if the user is a member of the chat they will also be removed from the chat**. If you don't want this, use the parameter *only_if_banned*

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#unbanchatmember  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| userId | <code>Number</code> | Unique identifier of the target user |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+restrictChatMember"></a>

### telegramBot.restrictChatMember(chatId, userId, [options]) ⇒ <code>Promise</code>
Use this method to restrict a user in a supergroup.The bot **must be an administrator in the supergroup** for this to workand must have the appropriate admin rights. Pass True for all boolean parametersto lift restrictions from a user. Returns True on success.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#restrictchatmember  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| userId | <code>Number</code> | Unique identifier of the target user |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+promoteChatMember"></a>

### telegramBot.promoteChatMember(chatId, userId, [options]) ⇒ <code>Promise</code>
Use this method to promote or demote a user in a supergroup or a channel.The bot **must be an administrator** in the chat for this to workand must have the appropriate admin rights. Pass False for all boolean parameters to demote a user.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success.  
**See**: https://core.telegram.org/bots/api#promotechatmember  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| userId | <code>Number</code> |  |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setChatAdministratorCustomTitle"></a>

### telegramBot.setChatAdministratorCustomTitle(chatId, userId, customTitle, [options]) ⇒ <code>Promise</code>
Use this method to set a custom title for an administrator in a supergroup promoted by the bot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setchatadministratorcustomtitle  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| userId | <code>Number</code> | Unique identifier of the target user |
| customTitle | <code>String</code> | New custom title for the administrator; 0-16 characters, emoji are not allowed |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+banChatSenderChat"></a>

### telegramBot.banChatSenderChat(chatId, senderChatId, [options]) ⇒ <code>Promise</code>
Use this method to ban a channel chat in a supergroup or a channel.Until the chat is [unbanned](https://core.telegram.org/bots/api#unbanchatsenderchat), the owner of the banned chat won't be able to send messages on behalf of any of their channels.The bot **must be an administrator in the supergroup or channel** for this to work and must have the appropriate administrator rights

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success.  
**See**: https://core.telegram.org/bots/api#banchatsenderchat  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| senderChatId | <code>Number</code> | Unique identifier of the target user |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+unbanChatSenderChat"></a>

### telegramBot.unbanChatSenderChat(chatId, senderChatId, [options]) ⇒ <code>Promise</code>
Use this method to unban a previously banned channel chat in a supergroup or channel.The bot **must be an administrator** for this to work and must have the appropriate administrator rights.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#unbanchatsenderchat  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| senderChatId | <code>Number</code> | Unique identifier of the target user |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setChatPermissions"></a>

### telegramBot.setChatPermissions(chatId, chatPermissions, [options]) ⇒ <code>Promise</code>
Use this method to set default chat permissions for all members.The bot **must be an administrator in the group or a supergroup** for this towork and **must have the `can_restrict_members` admin rights.**

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setchatpermissions  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| chatPermissions | <code>Array</code> | New default chat permissions |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+exportChatInviteLink"></a>

### telegramBot.exportChatInviteLink(chatId, [options]) ⇒ <code>Promise</code>
Use this method to generate a new primary invite link for a chat. **Any previously generated primary link is revoked**.The bot **must be an administrator in the chat** for this to work and must have the appropriate administrator rights.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - Exported invite link as String on success.  
**See**: https://core.telegram.org/bots/api#exportchatinvitelink  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+createChatInviteLink"></a>

### telegramBot.createChatInviteLink(chatId, [options]) ⇒ <code>Object</code>
Use this method to create an additional invite link for a chat.The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.The link generated with this method can be revoked using the method [revokeChatInviteLink](https://core.telegram.org/bots/api#revokechatinvitelink)

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Object</code> - The new invite link as [ChatInviteLink](https://core.telegram.org/bots/api#chatinvitelink) object  
**See**: https://core.telegram.org/bots/api#createchatinvitelink  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+editChatInviteLink"></a>

### telegramBot.editChatInviteLink(chatId, inviteLink, [options]) ⇒ <code>Promise</code>
Use this method to edit a non-primary invite link created by the bot.The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - The edited invite link as a [ChatInviteLink](https://core.telegram.org/bots/api#chatinvitelink) object  
**See**: https://core.telegram.org/bots/api#editchatinvitelink  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| inviteLink | <code>String</code> | Text with the invite link to edit |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+createChatSubscriptionInviteLink"></a>

### telegramBot.createChatSubscriptionInviteLink(chatId, subscriptionPeriod, subscriptionPrice, [options]) ⇒ <code>Promise</code>
Use this method to create a subscription invite link for a channel chat.The bot must have the can_invite_users administrator rights

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - The new invite link as a [ChatInviteLink](https://core.telegram.org/bots/api#chatinvitelink) object  
**See**: https://core.telegram.org/bots/api#createchatsubscriptioninvitelink  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| subscriptionPeriod | <code>Number</code> | The number of seconds the subscription will be active for before the next payment. Currently, it must always be 2592000 (30 days) |
| subscriptionPrice | <code>Number</code> | The amount of Telegram Stars a user must pay initially and after each subsequent subscription period to be a member of the chat (1-2500) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+editChatSubscriptionInviteLink"></a>

### telegramBot.editChatSubscriptionInviteLink(chatId, inviteLink, [options]) ⇒ <code>Promise</code>
Use this method to edit a subscription invite link created by the bot.The bot must have the can_invite_users administrator rights

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - The new invite link as a [ChatInviteLink](https://core.telegram.org/bots/api#chatinvitelink) object  
**See**: https://core.telegram.org/bots/api#editchatsubscriptioninvitelink  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| inviteLink | <code>String</code> | The invite link to edit |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+revokeChatInviteLink"></a>

### telegramBot.revokeChatInviteLink(chatId, inviteLink, [options]) ⇒ <code>Promise</code>
Use this method to revoke an invite link created by the bot.Note: If the primary link is revoked, a new link is automatically generatedThe bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - The revoked invite link as [ChatInviteLink](https://core.telegram.org/bots/api#chatinvitelink) object  
**See**: https://core.telegram.org/bots/api#revokechatinvitelink  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| inviteLink | <code>String</code> | The invite link to revoke |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+approveChatJoinRequest"></a>

### telegramBot.approveChatJoinRequest(chatId, userId, [options]) ⇒ <code>Promise</code>
Use this method to approve a chat join request.The bot **must be an administrator in the chat** for this to work and **must have the `can_invite_users` administrator right.**

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#approvechatjoinrequest  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| userId | <code>Number</code> | Unique identifier of the target user |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+declineChatJoinRequest"></a>

### telegramBot.declineChatJoinRequest(chatId, userId, [options]) ⇒ <code>Promise</code>
Use this method to decline a chat join request.The bot **must be an administrator in the chat** for this to work and **must have the `can_invite_users` administrator right**.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#declinechatjoinrequest  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| userId | <code>Number</code> | Unique identifier of the target user |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setChatPhoto"></a>

### telegramBot.setChatPhoto(chatId, photo, [options], [fileOptions]) ⇒ <code>Promise</code>
Use this method to set a new profile photo for the chat. **Photos can't be changed for private chats**.The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setchatphoto  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| photo | <code>stream.Stream</code> \| <code>Buffer</code> | A file path or a Stream. |
| [options] | <code>Object</code> | Additional Telegram query options |
| [fileOptions] | <code>Object</code> | Optional file related meta-data |

<a name="TelegramBot+deleteChatPhoto"></a>

### telegramBot.deleteChatPhoto(chatId, [options]) ⇒ <code>Promise</code>
Use this method to delete a chat photo. **Photos can't be changed for private chats**.The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#deletechatphoto  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setChatTitle"></a>

### telegramBot.setChatTitle(chatId, title, [options]) ⇒ <code>Promise</code>
Use this method to change the title of a chat. **Titles can't be changed for private chats**.The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setchattitle  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| title | <code>String</code> | New chat title, 1-255 characters |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setChatDescription"></a>

### telegramBot.setChatDescription(chatId, description, [options]) ⇒ <code>Promise</code>
Use this method to change the description of a group, a supergroup or a channel.The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setchatdescription  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| description | <code>String</code> | New chat title, 0-255 characters |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+pinChatMessage"></a>

### telegramBot.pinChatMessage(chatId, messageId, [options]) ⇒ <code>Promise</code>
Use this method to pin a message in a supergroup.If the chat is not a private chat, the **bot must be an administrator in the chat** for this to work and must have the `can_pin_messages` administratorright in a supergroup or `can_edit_messages` administrator right in a channel.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#pinchatmessage  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| messageId | <code>Number</code> | Identifier of a message to pin |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+unpinChatMessage"></a>

### telegramBot.unpinChatMessage(chatId, [options]) ⇒ <code>Promise</code>
Use this method to remove a message from the list of pinned messages in a chatIf the chat is not a private chat, the **bot must be an administrator in the chat** for this to work and must have the `can_pin_messages` administratorright in a supergroup or `can_edit_messages` administrator right in a channel.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#unpinchatmessage  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+unpinAllChatMessages"></a>

### telegramBot.unpinAllChatMessages(chatId, [options]) ⇒ <code>Promise</code>
Use this method to clear the list of pinned messages in a chat.If the chat is not a private chat, the **bot must be an administrator in the chat** for this to work and must have the `can_pin_messages` administratorright in a supergroup or `can_edit_messages` administrator right in a channel.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#unpinallchatmessages  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+leaveChat"></a>

### telegramBot.leaveChat(chatId, [options]) ⇒ <code>Promise</code>
Use this method for your bot to leave a group, supergroup or channel

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#leavechat  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getChat"></a>

### telegramBot.getChat(chatId, [options]) ⇒ <code>Promise</code>
Use this method to get up to date information about the chat(current name of the user for one-on-one conversations, currentusername of a user, group or channel, etc.).

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - [ChatFullInfo](https://core.telegram.org/bots/api#chatfullinfo) object on success  
**See**: https://core.telegram.org/bots/api#getchat  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) or channel |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getChatAdministrators"></a>

### telegramBot.getChatAdministrators(chatId, [options]) ⇒ <code>Promise</code>
Use this method to get a list of administrators in a chat

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, returns an Array of [ChatMember](https://core.telegram.org/bots/api#chatmember) objects that contains information about all chat administrators except other bots.If the chat is a group or a supergroup and no administrators were appointed, only the creator will be returned  
**See**: https://core.telegram.org/bots/api#getchatadministrators  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getChatMemberCount"></a>

### telegramBot.getChatMemberCount(chatId, [options]) ⇒ <code>Promise</code>
Use this method to get the number of members in a chat.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - Int on success  
**See**: https://core.telegram.org/bots/api#getchatmembercount  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getChatMember"></a>

### telegramBot.getChatMember(chatId, userId, [options]) ⇒ <code>Promise</code>
Use this method to get information about a member of a chat.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - [ChatMember](https://core.telegram.org/bots/api#chatmember) object on success  
**See**: https://core.telegram.org/bots/api#getchatmember  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup |
| userId | <code>Number</code> | Unique identifier of the target user |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setChatStickerSet"></a>

### telegramBot.setChatStickerSet(chatId, stickerSetName, [options]) ⇒ <code>Promise</code>
Use this method to set a new group sticker set for a supergroup.The bot **must be an administrator in the chat** for this to work and must have the appropriate administrator rights.**Note:** Use the field `can_set_sticker_set` optionally returned in [getChat](https://core.telegram.org/bots/api#getchat) requests to check if the bot can use this method.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setchatstickerset  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| stickerSetName | <code>String</code> | Name of the sticker set to be set as the group sticker set |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+deleteChatStickerSet"></a>

### telegramBot.deleteChatStickerSet(chatId, [options]) ⇒ <code>Promise</code>
Use this method to delete a group sticker set from a supergroup.Use the field `can_set_sticker_set` optionally returned in [getChat](https://core.telegram.org/bots/api#getchat) requests to check if the bot can use this method.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#deletechatstickerset  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getForumTopicIconStickers"></a>

### telegramBot.getForumTopicIconStickers(chatId, [options]) ⇒ <code>Promise</code>
Use this method to get custom emoji stickers, which can be used as a forum topic icon by any user.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - Array of [Sticker](https://core.telegram.org/bots/api#sticker) objects  
**See**: https://core.telegram.org/bots/api#getforumtopiciconstickers  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+createForumTopic"></a>

### telegramBot.createForumTopic(chatId, name, [options])
Use this method to create a topic in a forum supergroup chat.The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights.Returns information about the created topic as a [ForumTopic](https://core.telegram.org/bots/api#forumtopic) object.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**See**: https://core.telegram.org/bots/api#createforumtopic  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| name | <code>String</code> | Topic name, 1-128 characters |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+editForumTopic"></a>

### telegramBot.editForumTopic(chatId, messageThreadId, [options]) ⇒ <code>Promise</code>
Use this method to edit name and icon of a topic in a forum supergroup chat.The bot must be an administrator in the chat for this to work and must have can_manage_topics administrator rights, unless it is the creator of the topic.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#editforumtopic  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| messageThreadId | <code>Number</code> | Unique identifier for the target message thread of the forum topic |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+closeForumTopic"></a>

### telegramBot.closeForumTopic(chatId, messageThreadId, [options]) ⇒ <code>Promise</code>
Use this method to close an open topic in a forum supergroup chat.The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights, unless it is the creator of the topic.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#closeforumtopic  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| messageThreadId | <code>Number</code> | Unique identifier for the target message thread of the forum topic |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+reopenForumTopic"></a>

### telegramBot.reopenForumTopic(chatId, messageThreadId, [options]) ⇒ <code>Promise</code>
Use this method to reopen a closed topic in a forum supergroup chat.The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights, unless it is the creator of the topic.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#reopenforumtopic  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| messageThreadId | <code>Number</code> | Unique identifier for the target message thread of the forum topic |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+deleteForumTopic"></a>

### telegramBot.deleteForumTopic(chatId, messageThreadId, [options]) ⇒ <code>Promise</code>
Use this method to delete a forum topic along with all its messages in a forum supergroup chat.The bot must be an administrator in the chat for this to work and must have the can_delete_messages administrator rights.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#deleteforumtopic  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| messageThreadId | <code>Number</code> | Unique identifier for the target message thread of the forum topic |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+unpinAllForumTopicMessages"></a>

### telegramBot.unpinAllForumTopicMessages(chatId, messageThreadId, [options]) ⇒ <code>Promise</code>
Use this method to clear the list of pinned messages in a forum topic.The bot must be an administrator in the chat for this to work and must have the can_pin_messages administrator right in the supergroup.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#unpinallforumtopicmessages  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| messageThreadId | <code>Number</code> | Unique identifier for the target message thread of the forum topic |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+editGeneralForumTopic"></a>

### telegramBot.editGeneralForumTopic(chatId, name, [options]) ⇒ <code>Promise</code>
Use this method to edit the name of the 'General' topic in a forum supergroup chat.The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights.The topic will be automatically unhidden if it was hidden.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#editgeneralforumtopic  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| name | <code>String</code> | New topic name, 1-128 characters |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+closeGeneralForumTopic"></a>

### telegramBot.closeGeneralForumTopic(chatId, [options]) ⇒ <code>Promise</code>
Use this method to close an open 'General' topic in a forum supergroup chat.The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights.The topic will be automatically unhidden if it was hidden.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#closegeneralforumtopic  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+reopenGeneralForumTopic"></a>

### telegramBot.reopenGeneralForumTopic(chatId, [options]) ⇒ <code>Promise</code>
Use this method to reopen a closed 'General' topic in a forum supergroup chat.The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights.The topic will be automatically unhidden if it was hidden.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#reopengeneralforumtopic  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+hideGeneralForumTopic"></a>

### telegramBot.hideGeneralForumTopic(chatId, [options]) ⇒ <code>Promise</code>
Use this method to hide the 'General' topic in a forum supergroup chat.The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights.The topic will be automatically closed if it was open.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#hidegeneralforumtopic  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+unhideGeneralForumTopic"></a>

### telegramBot.unhideGeneralForumTopic(chatId, [options]) ⇒ <code>Promise</code>
Use this method to unhide the 'General' topic in a forum supergroup chat.The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#unhidegeneralforumtopic  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+unpinAllGeneralForumTopicMessages"></a>

### telegramBot.unpinAllGeneralForumTopicMessages(chatId, [options]) ⇒ <code>Promise</code>
Use this method to clear the list of pinned messages in a General forum topic.The bot must be an administrator in the chat for this to work and must have the can_pin_messages administrator right in the supergroup.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#unpinallgeneralforumtopicmessages  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername) |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+answerCallbackQuery"></a>

### telegramBot.answerCallbackQuery(callbackQueryId, [options]) ⇒ <code>Promise</code>
Use this method to send answers to callback queries sent from[inline keyboards](https://core.telegram.org/bots#inline-keyboards-and-on-the-fly-updating).The answer will be displayed to the user as a notification at the top of the chat screen or as an alert.This method has **older, compatible signatures ([1][answerCallbackQuery-v0.27.1])([2][answerCallbackQuery-v0.29.0])**that are being deprecated.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#answercallbackquery  

| Param | Type | Description |
| --- | --- | --- |
| callbackQueryId | <code>String</code> | Unique identifier for the query to be answered |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+savePreparedInlineMessage"></a>

### telegramBot.savePreparedInlineMessage(userId, result, [form]) ⇒ <code>Promise</code>
Use this method to get the unique identifier of the prepared message and expiration date of the prepared message as an object.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, returns a PreparedInlineMessage object  
**See**: https://core.telegram.org/bots/api#savepreparedinlinemessage  

| Param | Type | Default | Description |
| --- | --- | --- | --- |
| userId | <code>Number</code> |  | Unique identifier of the target user |
| result | <code>Object</code> |  | The prepared inline message result to be saved |
| [form] | <code>Object</code> | <code>{}</code> | Optional form data to include in the request |

<a name="TelegramBot+getUserChatBoosts"></a>

### telegramBot.getUserChatBoosts(chatId, userId, [options]) ⇒ <code>Promise</code>
Use this method to get the list of boosts added to a chat by a use.Requires administrator rights in the chat

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, returns a [UserChatBoosts](https://core.telegram.org/bots/api#userchatboosts) object  
**See**: https://core.telegram.org/bots/api#getuserchatboosts  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the group/channel |
| userId | <code>Number</code> | Unique identifier of the target user |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getBusinessConnection"></a>

### telegramBot.getBusinessConnection(businessConnectionId, [options]) ⇒ <code>Promise</code>
Use this method to get information about the connection of the bot with a business account

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, returns [BusinessConnection](https://core.telegram.org/bots/api#businessconnection) object  
**See**: https://core.telegram.org/bots/api#getbusinessconnection  

| Param | Type | Description |
| --- | --- | --- |
| businessConnectionId | <code>Number</code> \| <code>String</code> | Unique identifier for the group/channel |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setMyCommands"></a>

### telegramBot.setMyCommands(commands, [options]) ⇒ <code>Promise</code>
Use this method to change the list of the bot's commands.See https://core.telegram.org/bots#commands for more details about bot commands

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setmycommands  

| Param | Type | Description |
| --- | --- | --- |
| commands | <code>Array</code> | List of bot commands to be set as the list of the [bot's commands](https://core.telegram.org/bots/api#botcommand). At most 100 commands can be specified. |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+deleteMyCommands"></a>

### telegramBot.deleteMyCommands([options]) ⇒ <code>Promise</code>
Use this method to delete the list of the bot's commands for the given scope and user language. After deletion, [higher level commands](https://core.telegram.org/bots/api#determining-list-of-commands) will be shown to affected users.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#deletemycommands  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getMyCommands"></a>

### telegramBot.getMyCommands([options]) ⇒ <code>Promise</code>
Use this method to get the current list of the bot's commands for the given scope and user language.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - Array of [BotCommand](https://core.telegram.org/bots/api#botcommand) on success. If commands aren't set, an empty list is returned.  
**See**: https://core.telegram.org/bots/api#getmycommands  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setMyName"></a>

### telegramBot.setMyName([options]) ⇒ <code>Promise</code>
Use this method to change the bot's name.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setmyname  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getMyName"></a>

### telegramBot.getMyName([options]) ⇒ <code>Promise</code>
Use this method to get the current bot name for the given user language.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - [BotName](https://core.telegram.org/bots/api#botname) on success  
**See**: https://core.telegram.org/bots/api#getmyname  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setMyDescription"></a>

### telegramBot.setMyDescription([options]) ⇒ <code>Promise</code>
Use this method to change the bot's description, which is shown in the chat with the bot if the chat is empty.Returns True on success.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setmydescription  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getMyDescription"></a>

### telegramBot.getMyDescription([options]) ⇒ <code>Promise</code>
Use this method to get the current bot description for the given user language.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - Returns [BotDescription](https://core.telegram.org/bots/api#botdescription) on success.  
**See**: https://core.telegram.org/bots/api#getmydescription  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setMyShortDescription"></a>

### telegramBot.setMyShortDescription([options]) ⇒ <code>Promise</code>
Use this method to change the bot's short description, which is shown on the bot's profile pageand is sent together with the link when users share the bot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - Returns True on success.  
**See**: https://core.telegram.org/bots/api#setmyshortdescription  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getMyShortDescription"></a>

### telegramBot.getMyShortDescription([options]) ⇒ <code>Promise</code>
Use this method to get the current bot short description for the given user language.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - Returns [BotShortDescription](https://core.telegram.org/bots/api#botshortdescription) on success.  
**See**: https://core.telegram.org/bots/api#getmyshortdescription  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setChatMenuButton"></a>

### telegramBot.setChatMenuButton([options]) ⇒ <code>Promise</code>
Use this method to change the bot's menu button in a private chat, or the default menu button.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setchatmenubutton  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getChatMenuButton"></a>

### telegramBot.getChatMenuButton([options]) ⇒ <code>Promise</code>
Use this method to get the current value of the bot's menu button in a private chat, or the default menu button.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - [MenuButton](https://core.telegram.org/bots/api#menubutton) on success  
**See**: https://core.telegram.org/bots/api#getchatmenubutton  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setMyDefaultAdministratorRights"></a>

### telegramBot.setMyDefaultAdministratorRights([options]) ⇒ <code>Promise</code>
Use this method to change the default administrator rights requested by the bot when it's added as an administrator to groups or channels.These rights will be suggested to users, but they are are free to modify the list before adding the bot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#getchatmenubutton  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getMyDefaultAdministratorRights"></a>

### telegramBot.getMyDefaultAdministratorRights([options]) ⇒ <code>Promise</code>
Use this method to get the current default administrator rights of the bot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - [ChatAdministratorRights](https://core.telegram.org/bots/api#chatadministratorrights) on success  
**See**: https://core.telegram.org/bots/api#getmydefaultadministratorrights  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+editMessageText"></a>

### telegramBot.editMessageText(text, [options]) ⇒ <code>Promise</code>
Use this method to edit text or [game](https://core.telegram.org/bots/api#games) messages sent by the bot or via the bot (for inline bots).Note: that **you must provide one of chat_id, message_id, or inline_message_id** in your request.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, if the edited message is not an inline message, the edited [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned  
**See**: https://core.telegram.org/bots/api#editmessagetext  

| Param | Type | Description |
| --- | --- | --- |
| text | <code>String</code> | New text of the message |
| [options] | <code>Object</code> | Additional Telegram query options (provide either one of chat_id, message_id, or inline_message_id here) |

<a name="TelegramBot+editMessageCaption"></a>

### telegramBot.editMessageCaption(caption, [options]) ⇒ <code>Promise</code>
Use this method to edit captions of messages sent by the bot or via the bot (for inline bots).Note: You **must provide one of chat_id, message_id, or inline_message_id** in your request.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, if the edited message is not an inline message, the edited [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned  
**See**: https://core.telegram.org/bots/api#editmessagecaption  

| Param | Type | Description |
| --- | --- | --- |
| caption | <code>String</code> | New caption of the message |
| [options] | <code>Object</code> | Additional Telegram query options (provide either one of chat_id, message_id, or inline_message_id here) |

<a name="TelegramBot+editMessageMedia"></a>

### telegramBot.editMessageMedia(media, [options]) ⇒ <code>Promise</code>
Use this method to edit animation, audio, document, photo, or video messages.If a message is a part of a message album, then it can be edited only to a photo or a video.Otherwise, message type can be changed arbitrarily. When inline message is edited, new file can't be uploaded.Use previously uploaded file via its file_id or specify a URL.Note: You **must provide one of chat_id, message_id, or inline_message_id** in your request.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, if the edited message is not an inline message, the edited [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned  
**See**: https://core.telegram.org/bots/api#editmessagemedia  

| Param | Type | Description |
| --- | --- | --- |
| media | <code>Object</code> | A JSON-serialized object for a new media content of the message |
| [options] | <code>Object</code> | Additional Telegram query options (provide either one of chat_id, message_id, or inline_message_id here) |

<a name="TelegramBot+editMessageReplyMarkup"></a>

### telegramBot.editMessageReplyMarkup(replyMarkup, [options]) ⇒ <code>Promise</code>
Use this method to edit only the reply markup of messages sent by the bot or via the bot (for inline bots).Note: You **must provide one of chat_id, message_id, or inline_message_id** in your request.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, if the edited message is not an inline message, the edited [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned  
**See**: https://core.telegram.org/bots/api#editmessagetext  

| Param | Type | Description |
| --- | --- | --- |
| replyMarkup | <code>Object</code> | A JSON-serialized object for an inline keyboard. |
| [options] | <code>Object</code> | Additional Telegram query options (provide either one of chat_id, message_id, or inline_message_id here) |

<a name="TelegramBot+stopPoll"></a>

### telegramBot.stopPoll(chatId, pollId, [options]) ⇒ <code>Promise</code>
Use this method to stop a poll which was sent by the bot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the stopped [Poll](https://core.telegram.org/bots/api#poll) is returned  
**See**: https://core.telegram.org/bots/api#stoppoll  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the group/channel |
| pollId | <code>Number</code> | Identifier of the original message with the poll |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+sendSticker"></a>

### telegramBot.sendSticker(chatId, sticker, [options], [fileOptions]) ⇒ <code>Promise</code>
Use this method to send static .WEBP, [animated](https://telegram.org/blog/animated-stickers) .TGS,or [video](https://telegram.org/blog/video-stickers-better-reactions) .WEBM stickers.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) is returned  
**See**: https://core.telegram.org/bots/api#sendsticker  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| sticker | <code>String</code> \| <code>stream.Stream</code> \| <code>Buffer</code> | A file path, Stream or Buffer. Can also be a `file_id` previously uploaded. Stickers are WebP format files. |
| [options] | <code>Object</code> | Additional Telegram query options |
| [fileOptions] | <code>Object</code> | Optional file related meta-data |

<a name="TelegramBot+getStickerSet"></a>

### telegramBot.getStickerSet(name, [options]) ⇒ <code>Promise</code>
Use this method to get a sticker set.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, a [StickerSet](https://core.telegram.org/bots/api#stickerset) object is returned  
**See**: https://core.telegram.org/bots/api#getstickerset  

| Param | Type | Description |
| --- | --- | --- |
| name | <code>String</code> | Name of the sticker set |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getCustomEmojiStickers"></a>

### telegramBot.getCustomEmojiStickers(custom_emoji_ids, [options]) ⇒ <code>Promise</code>
Use this method to get information about custom emoji stickers by their identifiers.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - Array of [Sticker](https://core.telegram.org/bots/api#sticker) objects.  
**See**: https://core.telegram.org/bots/api#getcustomemojistickers  

| Param | Type | Description |
| --- | --- | --- |
| custom_emoji_ids | <code>Array</code> | List of custom emoji identifiers. At most 200 custom emoji identifiers can be specified. |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+uploadStickerFile"></a>

### telegramBot.uploadStickerFile(userId, sticker, stickerFormat, [options], [fileOptions]) ⇒ <code>Promise</code>
Use this method to upload a file with a sticker for later use in *createNewStickerSet* and *addStickerToSet* methods (can be used multipletimes).

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, a [File](https://core.telegram.org/bots/api#file) object is returned  
**See**: https://core.telegram.org/bots/api#uploadstickerfile  

| Param | Type | Default | Description |
| --- | --- | --- | --- |
| userId | <code>Number</code> |  | User identifier of sticker file owner |
| sticker | <code>String</code> \| <code>stream.Stream</code> \| <code>Buffer</code> |  | A file path or a Stream with the sticker in .WEBP, .PNG, .TGS, or .WEBM format. Can also be a `file_id` previously uploaded. |
| stickerFormat | <code>String</code> | <code>static</code> | Allow values:  `static`, `animated` or `video` |
| [options] | <code>Object</code> |  | Additional Telegram query options |
| [fileOptions] | <code>Object</code> |  | Optional file related meta-data |

<a name="TelegramBot+createNewStickerSet"></a>

### telegramBot.createNewStickerSet(userId, name, title, pngSticker, emojis, [options], [fileOptions]) ⇒ <code>Promise</code>
Use this method to create new sticker set owned by a user.The bot will be able to edit the created sticker set.You must use exactly one of the fields *png_sticker*, *tgs_sticker*, or *webm_sticker*

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#createnewstickerset  

| Param | Type | Description |
| --- | --- | --- |
| userId | <code>Number</code> | User identifier of created sticker set owner |
| name | <code>String</code> | Short name of sticker set, to be used in `t.me/addstickers/` URLs (e.g.,   *"animals"*). Can contain only english letters, digits and underscores.  Must begin with a letter, can't contain consecutive underscores and must end in `"_by_<bot_username>"`. `<bot_username>` is case insensitive. 1-64 characters. |
| title | <code>String</code> | Sticker set title, 1-64 characters |
| pngSticker | <code>String</code> \| <code>stream.Stream</code> \| <code>Buffer</code> | Png image with the sticker, must be up to 512 kilobytes in size,  dimensions must not exceed 512px, and either width or height must be exactly 512px. |
| emojis | <code>String</code> | One or more emoji corresponding to the sticker |
| [options] | <code>Object</code> | Additional Telegram query options |
| [fileOptions] | <code>Object</code> | Optional file related meta-data |

<a name="TelegramBot+addStickerToSet"></a>

### telegramBot.addStickerToSet(userId, name, sticker, emojis, stickerType, [options], [fileOptions]) ⇒ <code>Promise</code>
Use this method to add a new sticker to a set created by the bot.You must use exactly one of the fields *png_sticker*, *tgs_sticker*, or *webm_sticker*Animated stickers can be added to animated sticker sets and only to themNote:- Emoji sticker sets can have up to 200 sticker- Static or Animated sticker sets can have up to 120 stickers

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#addstickertoset  

| Param | Type | Default | Description |
| --- | --- | --- | --- |
| userId | <code>Number</code> |  | User identifier of sticker set owner |
| name | <code>String</code> |  | Sticker set name |
| sticker | <code>String</code> \| <code>stream.Stream</code> \| <code>Buffer</code> |  | Png image with the sticker (must be up to 512 kilobytes in size, dimensions must not exceed 512px, and either width or height must be exactly 512px, [TGS animation](https://core.telegram.org/stickers#animated-sticker-requirements) with the sticker or [WEBM video](https://core.telegram.org/stickers#video-sticker-requirements) with the sticker. |
| emojis | <code>String</code> |  | One or more emoji corresponding to the sticker |
| stickerType | <code>String</code> | <code>png_sticker</code> | Allow values: `png_sticker`, `tgs_sticker`, or `webm_sticker`. |
| [options] | <code>Object</code> |  | Additional Telegram query options |
| [fileOptions] | <code>Object</code> |  | Optional file related meta-data |

<a name="TelegramBot+setStickerPositionInSet"></a>

### telegramBot.setStickerPositionInSet(sticker, position, [options]) ⇒ <code>Promise</code>
Use this method to move a sticker in a set created by the bot to a specific position.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setstickerpositioninset  

| Param | Type | Description |
| --- | --- | --- |
| sticker | <code>String</code> | File identifier of the sticker |
| position | <code>Number</code> | New sticker position in the set, zero-based |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+deleteStickerFromSet"></a>

### telegramBot.deleteStickerFromSet(sticker, [options]) ⇒ <code>Promise</code>
Use this method to delete a sticker from a set created by the bot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#deletestickerfromset  
**Todo**

- [ ] Add tests for this method!


| Param | Type | Description |
| --- | --- | --- |
| sticker | <code>String</code> | File identifier of the sticker |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+replaceStickerInSet"></a>

### telegramBot.replaceStickerInSet(user_id, name, sticker, [options]) ⇒ <code>Promise</code>
Use this method to replace an existing sticker in a sticker set with a new one

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#replacestickerinset  
**Todo**

- [ ] Add tests for this method!


| Param | Type | Description |
| --- | --- | --- |
| user_id | <code>Number</code> | User identifier of the sticker set owner |
| name | <code>String</code> | Sticker set name |
| sticker | <code>String</code> | File identifier of the sticker |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setStickerEmojiList"></a>

### telegramBot.setStickerEmojiList(sticker, emojiList, [options]) ⇒ <code>Promise</code>
Use this method to change the list of emoji assigned to a regular or custom emoji sticker.The sticker must belong to a sticker set created by the bot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setstickeremojilist  

| Param | Type | Description |
| --- | --- | --- |
| sticker | <code>String</code> | File identifier of the sticker |
| emojiList | <code>Array</code> | A JSON-serialized list of 1-20 emoji associated with the sticker |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setStickerKeywords"></a>

### telegramBot.setStickerKeywords(sticker, [options]) ⇒ <code>Promise</code>
Use this method to change the list of emoji assigned to a `regular` or `custom emoji` sticker.The sticker must belong to a sticker set created by the bot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setstickerkeywords  

| Param | Type | Description |
| --- | --- | --- |
| sticker | <code>String</code> | File identifier of the sticker |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setStickerMaskPosition"></a>

### telegramBot.setStickerMaskPosition(sticker, [options]) ⇒ <code>Promise</code>
Use this method to change the [mask position](https://core.telegram.org/bots/api#maskposition) of a mask sticker.The sticker must belong to a sticker set created by the bot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setstickermaskposition  

| Param | Type | Description |
| --- | --- | --- |
| sticker | <code>String</code> | File identifier of the sticker |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setStickerSetTitle"></a>

### telegramBot.setStickerSetTitle(name, title, [options]) ⇒ <code>Promise</code>
Use this method to set the title of a created sticker set.The sticker must belong to a sticker set created by the bot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setstickersettitle  

| Param | Type | Description |
| --- | --- | --- |
| name | <code>String</code> | Sticker set name |
| title | <code>String</code> | Sticker set title, 1-64 characters |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setStickerSetThumbnail"></a>

### telegramBot.setStickerSetThumbnail(userId, name, thumbnail, [options], [fileOptions]) ⇒ <code>Promise</code>
Use this method to add a thumb to a set created by the bot.Animated thumbnails can be set for animated sticker sets only. Video thumbnails can be set only for video sticker sets only

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setstickersetthumbnail  

| Param | Type | Description |
| --- | --- | --- |
| userId | <code>Number</code> | User identifier of sticker set owner |
| name | <code>String</code> | Sticker set name |
| thumbnail | <code>String</code> \| <code>stream.Stream</code> \| <code>Buffer</code> | A .WEBP or .PNG image with the thumbnail, must be up to 128 kilobytes in size and have width and height exactly 100px, a TGS animation with the thumbnail up to 32 kilobytes in size or a WEBM video with the thumbnail up to 32 kilobytes in size. Pass a file_id as a String to send a file that already exists on the Telegram servers, pass an HTTP URL as a String for Telegram to get a file from the Internet, or upload a new one. Animated sticker set thumbnails can't be uploaded via HTTP URL. |
| [options] | <code>Object</code> | Additional Telegram query options |
| [fileOptions] | <code>Object</code> | Optional file related meta-data |

<a name="TelegramBot+setCustomEmojiStickerSetThumbnail"></a>

### telegramBot.setCustomEmojiStickerSetThumbnail(name, [options]) ⇒ <code>Promise</code>
Use this method to set the thumbnail of a custom emoji sticker set.The sticker must belong to a sticker set created by the bot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#setcustomemojistickersetthumbnail  

| Param | Type | Description |
| --- | --- | --- |
| name | <code>String</code> | Sticker set name |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+deleteStickerSet"></a>

### telegramBot.deleteStickerSet(name, [options]) ⇒ <code>Promise</code>
Use this method to delete a sticker set that was created by the bot.The sticker must belong to a sticker set created by the bot.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#deletestickerset  

| Param | Type | Description |
| --- | --- | --- |
| name | <code>String</code> | Sticker set name |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+answerInlineQuery"></a>

### telegramBot.answerInlineQuery(inlineQueryId, results, [options]) ⇒ <code>Promise</code>
Send answers to an inline query.Note: No more than 50 results per query are allowed.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, True is returned  
**See**: https://core.telegram.org/bots/api#answerinlinequery  

| Param | Type | Description |
| --- | --- | --- |
| inlineQueryId | <code>String</code> | Unique identifier of the query |
| results | <code>[ &#x27;Array&#x27; ].&lt;InlineQueryResult&gt;</code> | An array of results for the inline query |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+answerWebAppQuery"></a>

### telegramBot.answerWebAppQuery(webAppQueryId, result, [options]) ⇒ <code>Promise</code>
Use this method to set the result of an interaction with a [Web App](https://core.telegram.org/bots/webapps)and send a corresponding message on behalf of the user to the chat from which the query originated.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, a [SentWebAppMessage](https://core.telegram.org/bots/api#sentwebappmessage) object is returned  
**See**: https://core.telegram.org/bots/api#answerwebappquery  

| Param | Type | Description |
| --- | --- | --- |
| webAppQueryId | <code>String</code> | Unique identifier for the query to be answered |
| result | <code>InlineQueryResult</code> | object that represents one result of an inline query |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+sendInvoice"></a>

### telegramBot.sendInvoice(chatId, title, description, payload, providerToken, currency, prices, [options]) ⇒ <code>Promise</code>
Use this method to send an invoice.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) is returned  
**See**: https://core.telegram.org/bots/api#sendinvoice  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| title | <code>String</code> | Product name, 1-32 characters |
| description | <code>String</code> | Product description, 1-255 characters |
| payload | <code>String</code> | Bot defined invoice payload, 1-128 bytes. This will not be displayed to the user, use for your internal processes. |
| providerToken | <code>String</code> | Payments provider token, obtained via `@BotFather` |
| currency | <code>String</code> | Three-letter ISO 4217 currency code |
| prices | <code>Array</code> | Breakdown of prices |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+createInvoiceLink"></a>

### telegramBot.createInvoiceLink(title, description, payload, providerToken, currency, prices, [options]) ⇒ <code>Promise</code>
Use this method to create a link for an invoice.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - The created invoice link as String on success.  
**See**: https://core.telegram.org/bots/api#createinvoicelink  

| Param | Type | Description |
| --- | --- | --- |
| title | <code>String</code> | Product name, 1-32 characters |
| description | <code>String</code> | Product description, 1-255 characters |
| payload | <code>String</code> | Bot defined invoice payload |
| providerToken | <code>String</code> | Payment provider token |
| currency | <code>String</code> | Three-letter ISO 4217 currency code |
| prices | <code>Array</code> | Breakdown of prices |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+answerShippingQuery"></a>

### telegramBot.answerShippingQuery(shippingQueryId, ok, [options]) ⇒ <code>Promise</code>
Use this method to reply to shipping queries.If you sent an invoice requesting a shipping address and the parameter is_flexible was specified,the Bot API will send an [Update](https://core.telegram.org/bots/api#update) with a shipping_query field to the bot

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, True is returned  
**See**: https://core.telegram.org/bots/api#answershippingquery  

| Param | Type | Description |
| --- | --- | --- |
| shippingQueryId | <code>String</code> | Unique identifier for the query to be answered |
| ok | <code>Boolean</code> | Specify if delivery of the product is possible |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+answerPreCheckoutQuery"></a>

### telegramBot.answerPreCheckoutQuery(preCheckoutQueryId, ok, [options]) ⇒ <code>Promise</code>
Use this method to respond to such pre-checkout queriesOnce the user has confirmed their payment and shipping details, the Bot API sends the final confirmation in the form ofan [Update](https://core.telegram.org/bots/api#update) with the field *pre_checkout_query*.**Note:** The Bot API must receive an answer within 10 seconds after the pre-checkout query was sent.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, True is returned  
**See**: https://core.telegram.org/bots/api#answerprecheckoutquery  

| Param | Type | Description |
| --- | --- | --- |
| preCheckoutQueryId | <code>String</code> | Unique identifier for the query to be answered |
| ok | <code>Boolean</code> | Specify if every order details are ok |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getStarTransactions"></a>

### telegramBot.getStarTransactions([options]) ⇒ <code>Promise</code>
Use this method for get the bot's Telegram Star transactions in chronological order

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, returns a [StarTransactions](https://core.telegram.org/bots/api#startransactions) object  
**See**: https://core.telegram.org/bots/api#getstartransactions  

| Param | Type | Description |
| --- | --- | --- |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+refundStarPayment"></a>

### telegramBot.refundStarPayment(userId, telegramPaymentChargeId, [options]) ⇒ <code>Promise</code>
Use this method for refund a successful payment in [Telegram Stars](https://t.me/BotNews/90)

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, True is returned  
**See**: https://core.telegram.org/bots/api#refundstarpayment  

| Param | Type | Description |
| --- | --- | --- |
| userId | <code>Number</code> | Unique identifier of the user whose payment will be refunded |
| telegramPaymentChargeId | <code>String</code> | Telegram payment identifier |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+sendGame"></a>

### telegramBot.sendGame(chatId, gameShortName, [options]) ⇒ <code>Promise</code>
Use this method to send a game.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, the sent [Message](https://core.telegram.org/bots/api#message) is returned  
**See**: https://core.telegram.org/bots/api#sendgame  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) |
| gameShortName | <code>String</code> | name of the game to be sent. Set up your games via `@BotFather`. |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+setGameScore"></a>

### telegramBot.setGameScore(userId, score, [options]) ⇒ <code>Promise</code>
Use this method to set the score of the specified user in a game message.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, if the message is not an inline message, the [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned  
**See**: https://core.telegram.org/bots/api#setgamescore  

| Param | Type | Description |
| --- | --- | --- |
| userId | <code>Number</code> | Unique identifier of the target user |
| score | <code>Number</code> | New score value, must be non-negative |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+getGameHighScores"></a>

### telegramBot.getGameHighScores(userId, [options]) ⇒ <code>Promise</code>
Use this method to get data for high score tables.Will return the score of the specified user and several of their neighbors in a game.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - On success, returns an Array of [GameHighScore](https://core.telegram.org/bots/api#gamehighscore) objects  
**See**: https://core.telegram.org/bots/api#getgamehighscores  

| Param | Type | Description |
| --- | --- | --- |
| userId | <code>Number</code> | Unique identifier of the target user |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+deleteMessage"></a>

### telegramBot.deleteMessage(chatId, messageId, [options]) ⇒ <code>Promise</code>
Use this method to delete a message, including service messages, with the following limitations:- A message can only be deleted if it was sent less than 48 hours ago.- A dice message can only be deleted if it was sent more than 24 hours ago.- Bots can delete outgoing messages in groups and supergroups.- Bots can delete incoming messages in groups, supergroups and channels.- Bots granted `can_post_messages` permissions can delete outgoing messages in channels.- If the bot is an administrator of a group, it can delete any message there.- If the bot has `can_delete_messages` permission in a supergroup, it can delete any message there.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>Promise</code> - True on success  
**See**: https://core.telegram.org/bots/api#deletemessage  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format @channelusername) |
| messageId | <code>Number</code> | Unique identifier of the target message |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot+deleteMessages"></a>

### telegramBot.deleteMessages(chatId, messageIds, [options]) ⇒ <code>[ &#x27;Promise&#x27; ].&lt;Boolean&gt;</code>
Use this method to delete multiple messages simultaneously. If some of the specified messages can't be found, they are skipped.

**Kind**: instance method of [<code>TelegramBot</code>](#TelegramBot)  
**Returns**: <code>[ &#x27;Promise&#x27; ].&lt;Boolean&gt;</code> - True on success  
**See**: https://core.telegram.org/bots/api#deletemessages  

| Param | Type | Description |
| --- | --- | --- |
| chatId | <code>Number</code> \| <code>String</code> | Unique identifier for the target chat or username of the target channel (in the format @channelusername) |
| messageIds | <code>[ &#x27;Array&#x27; ].&lt;(Number\|String)&gt;</code> | Identifiers of 1-100 messages to delete. See deleteMessage for limitations on which messages can be deleted |
| [options] | <code>Object</code> | Additional Telegram query options |

<a name="TelegramBot.errors"></a>

### TelegramBot.errors : <code>Object</code>
The different errors the library uses.

**Kind**: static property of [<code>TelegramBot</code>](#TelegramBot)  
<a name="TelegramBot.messageTypes"></a>

### TelegramBot.messageTypes : <code>[ &#x27;Array&#x27; ].&lt;String&gt;</code>
The types of message updates the library handles.

**Kind**: static property of [<code>TelegramBot</code>](#TelegramBot)  
* * *


[usage-sending-files-performance]:https://github.com/yagop/node-telegram-bot-api/tree/master/doc/usage.md#sending-files-performance
[setWebHook-v0.25.0]:https://github.com/yagop/node-telegram-bot-api/tree/4e5a493cadfaad5589a8d79e55d9e0d103000ce4#telegrambotsetwebhookurl-cert
[getUpdates-v0.25.0]:https://github.com/yagop/node-telegram-bot-api/tree/4e5a493cadfaad5589a8d79e55d9e0d103000ce4#TelegramBot+getUpdates
[getUserProfilePhotos-v0.25.0]:https://github.com/yagop/node-telegram-bot-api/tree/4e5a493cadfaad5589a8d79e55d9e0d103000ce4#TelegramBot+getUserProfilePhotos
[answerCallbackQuery-v0.27.1]:https://github.com/yagop/node-telegram-bot-api/blob/v0.27.1/doc/api.md#TelegramBot+answerCallbackQuery
[answerCallbackQuery-v0.29.0]:https://github.com/yagop/node-telegram-bot-api/blob/v0.29.0/doc/api.md#TelegramBot+answerCallbackQuery
