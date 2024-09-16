# Usage

- [Usage](#usage)
  - [Events](#events)
  - [WebHooks](#webhooks)
  - [Sending files](#sending-files)
    - [File Options (metadata)](#file-options-metadata)
    - [Performance Issue](#performance-issue)
  - [Error handling](#error-handling)
  - [Polling errors](#polling-errors)
  - [WebHook errors](#webhook-errors)

<a name="events"></a>
## Events

*TelegramBot* is an [EventEmitter](https://nodejs.org/api/events.html#events_class_eventemitter)
that emits the following events:

1. `message`: Received a new incoming [Message][message] of any kind
   1. Depending on the properties of the [Message][message], one of these
     events may **ALSO** be emitted: `text`, `audio`, `document`, `photo`,
     `sticker`, `video`, `voice`, `contact`, `location`,
     `new_chat_members`, `left_chat_member`, `new_chat_title`,
     `new_chat_photo`, `delete_chat_photo`, `group_chat_created`,
     `game`, `pinned_message`, `poll`, `dice`, `migrate_from_chat_id`, `migrate_to_chat_id`,
     `channel_chat_created`, `supergroup_chat_created`,
     `successful_payment`, `invoice`, `video_note`
   1. **Arguments**: `message` ([Message][message]), `metadata` (`{ type?:string }`)
   1. `new_chat_participant`, `left_chat_participant` are **deprecated**
1. `callback_query`: Received a new incoming [Callback Query][callback-query]
1. `inline_query`: Received a new incoming [Inline Query][inline-query]
1. `chosen_inline_result`: Received result of an inline query i.e. [ChosenInlineResult][chosen-inline-result]
1. `channel_post`: Received a new incoming channel post of any kind
1. `edited_message`: Received a new version of a message that is known to the bot and was edited \
   Depending on the properties of the [Message][message], one of these events may **ALSO** be emitted:
   1. `edited_message_text`
   1. `edited_message_caption`
1. `edited_channel_post`: Received a new version of a channel post that is known to the bot and was edited \
   Depending on the properties of the [Message][message], one of these events may **ALSO** be emitted:
   1. `edited_channel_post_text`
   1. `edited_channel_post_caption`
1. `shipping_query`: Received a new incoming shipping query
1. `pre_checkout_query`: Received a new  incoming pre-checkout query
1. `poll`: Received a new  incoming poll
2. `poll_answer`: A user has changed their answer in a non-anonymous poll (Only polls sent by the bot)
3. `chat_member`: A chat member's status was updated in a chat
4. `my_chat_member`: The bot's chat member status was updated in a chat
5. `chat_join_request`: A request to join the chat has been sent (The bot must have the can_invite_users administrator right)
5. `polling_error`: Error occurred during polling. See [polling errors](#polling-errors).
6. `webhook_error`: Error occurred handling a webhook request. See [webhook errors](#webhook-errors).
7. `error`: Unexpected error occurred, usually fatal!

**Tip:** Its much better to listen a specific event rather than on
`message` in order to stay safe from the content.

**Tip:** Bot must be enabled on [inline mode][inline-mode] for receive some
messages.

<a name="webhooks"></a>
## WebHooks

Telegram only supports HTTPS connections to WebHooks.
Therefore, in order to set a WebHook, you will need a SSL certificate.
Since August 29, 2015 Telegram supports self-signed ones, thus, you can
generate them:

```bash
# Our private cert will be key.pem, keep this file private
$ openssl genrsa -out key.pem 2048

# Our public certificate will be crt.pem
$ openssl req -new -sha256 -key key.pem -out crt.pem
```

Once they are generated, the `crt.pem` should be uploaded, when setting up
your webhook. For example,

```js
bot.setWebHook('public-url.com', {
  certificate: 'path/to/crt.pem', // Path to your crt.pem
});
```

**Note:** If you encounter an error, like
`Error: error:0906D06C:PEM routines:PEM_read_bio:no start line`,
you may want to proceed to [this issue][issue-63] for more information.

<a name="sending-files"></a>
## Sending files

The library makes it easy to get started sending files. *By default*, you
may provide a **file-path** and the library will handle reading it for you.
For example,

```js
bot.sendAudio(chatId, 'path/to/audio.mp3');
```

You may also pass in a **Readable Stream** from which data will be piped.
For example,

```js
const stream = fs.createReadStream('path/to/audio.mp3');
bot.sendAudio(chatId, stream);
```

You may also pass in a **Buffer** containing the contents of your file.
For example,

```js
const buffer = fs.readFileSync('path/to/audio.mp3'); // sync! that's sad! :-( Just making a point!
bot.sendAudio(chatId, buffer);
```

If you already have a **File ID**, you earlier retrieved from Telegram,
you may pass it in, for example:

```js
const fileId = getFileIdSomehow();
bot.sendAudio(chatId, fileId);
```

Some API methods, such as *SendPhoto*, allow passing a **HTTP URL**, that
the Telegram servers will use to download the file. For example,

```js
const url = 'https://telegram.org/img/t_logo.png';
bot.sendPhoto(chatId, url);
```

If you wish to explicitly specify the filename or
[MIME type](http://en.wikipedia.org/wiki/Internet_media_type),
you may pass an additional argument as file options, like so:

```js
const fileOptions = {
  // Explicitly specify the file name.
  filename: 'customfilename',
  // Explicitly specify the MIME type.
  contentType: 'audio/mpeg',
};
bot.sendAudio(chatId, data, {}, fileOptions);
```

**NOTE:** You **MUST** provide an empty object (`{}`) in place of
*Additional Telegram query options*, if you have **no** query options
to specify. For example,

```js
// WRONG!
// 'fileOptions' will be taken as additional Telegram query options!!!
bot.sendAudio(chatId, data, fileOptions);

// RIGHT!
bot.sendAudio(chatId, data, {}, fileOptions);
```


<a name="sending-files-options"></a>
### File Options (metadata)

When sending files, the library automatically resolves
the `filename` and `contentType` properties.
**For now, this has to be manually activated using environment
variable `NTBA_FIX_350`.**

In order of highest-to-lowest precedence in searching for
a value, when resolving the `filename`:

*(`fileOptions` is the Object argument passed to the method.
The "file" argument passed to the method can be a `Stream`,
`Buffer` or `filepath`.)*

1. Is `fileOptions.filename` explictly defined?
1. Does `Stream#path` exist?
1. Is `filepath` provided?
1. Default to `"filename"`

And the `contentType`:

1. Is `fileOptions.contentType` explictly-defined?
1. Does `Stream#path` exist?
1. Try detecting file-type from the `Buffer`
1. Is `filepath` provided?
1. Is `fileOptions.filename` explicitly defined?
1. Default to `"application/octet-stream"`

<a name="sending-files-performance"></a>
### Performance Issue

To support providing file-paths to methods that send files involves
performing a file operation, i.e. *fs.existsSync()*, that checks for
the existence of the file at the provided path. While the cost of
this operation *might* be negligible in most use cases, if you want
to squeeze the best performance out of this library, you may wish to
disable this behavior.

This will mean that you will **NOT** be able to pass in file-paths.
You will have to use Streams or Buffers to provide the file contents.

Disabling this behavior:

```js
const bot = new TelegramBot(token, {
  filepath: false,
});
```

<a name="error-handling"></a>
## Error handling

Every `Error` object we pass back has the properties:

* `code` (String):
  * value is `EFATAL` if error was fatal e.g. network error
  * value is `EPARSE` if response body could **not** be parsed
  * value is `ETELEGRAM` if error was returned from Telegram servers
* `response` ([http.IncomingMessage](https://nodejs.org/api/http.html#http_class_http_incomingmessage)):
  * available if `error.code` is **not** `EFATAL`
* `response.body` (String|Object): Error response from Telegram
  * type is `String` if `error.code` is `EPARSE`
  * type is `Object` if `error.code` is `ETELEGRAM`

For example, sending message to a non-existent user:

```js
bot.sendMessage(nonExistentUserId, 'text').catch((error) => {
  console.log(error.code);  // => 'ETELEGRAM'
  console.log(error.response.body); // => { ok: false, error_code: 400, description: 'Bad Request: chat not found' }
});
```

<a name="polling-errors"></a>
## Polling errors

An error may occur during polling. It is up to you to handle it
as you see fit. You may decide to crash your bot after a maximum number
of polling errors occurring. **It is all up to you.**

By default, the polling error is just logged to stderr, if you do
**not** handle this event yourself.

Listen on the `'polling_error'` event. For example,

```js
bot.on('polling_error', (error) => {
  console.log(error.code);  // => 'EFATAL'
});
```

<a name="webhook-errors"></a>
## WebHook errors

Just like with [polling errors](#polling-errors), you decide on how to
handle it. By default, the error is logged to stderr.

Listen on the `'webhook_error'` event. For example,

```js
bot.on('webhook_error', (error) => {
  console.log(error.code);  // => 'EPARSE'
});
```

[update]:https://core.telegram.org/bots/api#update
[message]:https://core.telegram.org/bots/api#message
[callback-query]:https://core.telegram.org/bots/api#callbackquery
[inline-query]:https://core.telegram.org/bots/api#inlinequery
[chosen-inline-result]:https://core.telegram.org/bots/api#choseninlineresult
[inline-mode]:https://core.telegram.org/bots/api#inline-mode
[issue-63]:https://github.com/yagop/node-telegram-bot-api/issues/63
