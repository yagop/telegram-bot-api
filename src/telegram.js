// shims
require('array.prototype.findindex').shim(); // for Node.js v0.x

const errors = require('./errors');
const TelegramBotWebHook = require('./telegramWebHook');
const TelegramBotPolling = require('./telegramPolling');
const debug = require('debug')('node-telegram-bot-api');
const EventEmitter = require('eventemitter3');
const fileType = require('file-type');
const request = require('@cypress/request-promise');
const streamedRequest = require('@cypress/request');
const qs = require('querystring');
const stream = require('stream');
const mime = require('mime');
const path = require('path');
const URL = require('url');
const fs = require('fs');
const pump = require('pump');
const deprecate = require('./utils').deprecate;

const _messageTypes = [
  'text',
  'animation',
  'audio',
  'channel_chat_created',
  'contact',
  'delete_chat_photo',
  'dice',
  'document',
  'game',
  'group_chat_created',
  'invoice',
  'left_chat_member',
  'location',
  'migrate_from_chat_id',
  'migrate_to_chat_id',
  'new_chat_members',
  'new_chat_photo',
  'new_chat_title',
  'passport_data',
  'photo',
  'pinned_message',
  'poll',
  'sticker',
  'successful_payment',
  'supergroup_chat_created',
  'video',
  'video_note',
  'voice',
  'video_chat_started',
  'video_chat_ended',
  'video_chat_participants_invited',
  'video_chat_scheduled',
  'message_auto_delete_timer_changed',
  'chat_invite_link',
  'chat_member_updated',
  'web_app_data',
  'message_reaction'
];

const _deprecatedMessageTypes = [
  'new_chat_participant', 'left_chat_participant'
];

/**
 * JSON-serialize data. If the provided data is already a String,
 * return it as is.
 * @private
 * @param  {*} data
 * @return {String}
 */
function stringify(data) {
  if (typeof data === 'string') {
    return data;
  }
  return JSON.stringify(data);
}


class TelegramBot extends EventEmitter {
  /**
   * The different errors the library uses.
   * @type {Object}
   */
  static get errors() {
    return errors;
  }

  /**
   * The types of message updates the library handles.
   * @type {String[]}
   */
  static get messageTypes() {
    return _messageTypes;
  }

  /**
   * Add listener for the specified [event](https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#events).
   * This is the usual `emitter.on()` method.
   * @param  {String} event
   * @param  {Function} listener
   * @see {@link https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#events|Available events}
   * @see https://nodejs.org/api/events.html#events_emitter_on_eventname_listener
   */
  on(event, listener) {
    if (_deprecatedMessageTypes.indexOf(event) !== -1) {
      const url = 'https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#events';
      deprecate(`Events ${_deprecatedMessageTypes.join(',')} are deprecated. See the updated list of events: ${url}`);
    }
    super.on(event, listener);
  }

  /**
   * Both request method to obtain messages are implemented. To use standard polling, set `polling: true`
   * on `options`. Notice that [webHook](https://core.telegram.org/bots/api#setwebhook) will need a SSL certificate.
   * Emits `message` when a message arrives.
   *
   * @class TelegramBot
   * @constructor
   * @param {String} token Bot Token
   * @param {Object} [options]
   * @param {Boolean|Object} [options.polling=false] Set true to enable polling or set options.
   *  If a WebHook has been set, it will be deleted automatically.
   * @param {String|Number} [options.polling.timeout=10] *Deprecated. Use `options.polling.params` instead*.
   *  Timeout in seconds for long polling.
   * @param {Boolean} [options.testEnvironment=false] Set true to  work with test enviroment.
   * When working with the test environment, you may use HTTP links without TLS to test your Web App.
   * @param {String|Number} [options.polling.interval=300] Interval between requests in miliseconds
   * @param {Boolean} [options.polling.autoStart=true] Start polling immediately
   * @param {Object} [options.polling.params] Parameters to be used in polling API requests.
   *  See https://core.telegram.org/bots/api#getupdates for more information.
   * @param  {Number} [options.polling.params.timeout=10] Timeout in seconds for long polling.
   * @param {Boolean|Object} [options.webHook=false] Set true to enable WebHook or set options
   * @param {String} [options.webHook.host="0.0.0.0"] Host to bind to
   * @param {Number} [options.webHook.port=8443] Port to bind to
   * @param {String} [options.webHook.key] Path to file with PEM private key for webHook server.
   *  The file is read **synchronously**!
   * @param {String} [options.webHook.cert] Path to file with PEM certificate (public) for webHook server.
   *  The file is read **synchronously**!
   * @param {String} [options.webHook.pfx] Path to file with PFX private key and certificate chain for webHook server.
   *  The file is read **synchronously**!
   * @param {Boolean} [options.webHook.autoOpen=true] Open webHook immediately
   * @param {Object} [options.webHook.https] Options to be passed to `https.createServer()`.
   *  Note that `options.webHook.key`, `options.webHook.cert` and `options.webHook.pfx`, if provided, will be
   *  used to override `key`, `cert` and `pfx` in this object, respectively.
   *  See https://nodejs.org/api/https.html#https_https_createserver_options_requestlistener for more information.
   * @param {String} [options.webHook.healthEndpoint="/healthz"] An endpoint for health checks that always responds with 200 OK
   * @param {Boolean} [options.onlyFirstMatch=false] Set to true to stop after first match. Otherwise, all regexps are executed
   * @param {Object} [options.request] Options which will be added for all requests to telegram api.
   *  See https://github.com/request/request#requestoptions-callback for more information.
   * @param {String} [options.baseApiUrl="https://api.telegram.org"] API Base URl; useful for proxying and testing
   * @param {Boolean} [options.filepath=true] Allow passing file-paths as arguments when sending files,
   *  such as photos using `TelegramBot#sendPhoto()`. See [usage information][usage-sending-files-performance]
   *  for more information on this option and its consequences.
   * @param {Boolean} [options.badRejection=false] Set to `true`
   *  **if and only if** the Node.js version you're using terminates the
   *  process on unhandled rejections. This option is only for
   *  *forward-compatibility purposes*.
   * @see https://core.telegram.org/bots/api
   */
  constructor(token, options = {}) {
    super();
    this.token = token;
    this.options = options;
    this.options.polling = (typeof options.polling === 'undefined') ? false : options.polling;
    this.options.webHook = (typeof options.webHook === 'undefined') ? false : options.webHook;
    this.options.baseApiUrl = options.baseApiUrl || 'https://api.telegram.org';
    this.options.filepath = (typeof options.filepath === 'undefined') ? true : options.filepath;
    this.options.badRejection = (typeof options.badRejection === 'undefined') ? false : options.badRejection;
    this._textRegexpCallbacks = [];
    this._replyListenerId = 0;
    this._replyListeners = [];
    this._polling = null;
    this._webHook = null;

    if (options.polling) {
      const autoStart = options.polling.autoStart;
      if (typeof autoStart === 'undefined' || autoStart === true) {
        this.startPolling();
      }
    }

    if (options.webHook) {
      const autoOpen = options.webHook.autoOpen;
      if (typeof autoOpen === 'undefined' || autoOpen === true) {
        this.openWebHook();
      }
    }
  }

  /**
   * Generates url with bot token and provided path/method you want to be got/executed by bot
   * @param  {String} path
   * @return {String} url
   * @private
   * @see https://core.telegram.org/bots/api#making-requests
   */
  _buildURL(_path) {
    return `${this.options.baseApiUrl}/bot${this.token}${this.options.testEnvironment ? '/test' : ''}/${_path}`;
  }

  /**
   * Fix 'reply_markup' parameter by making it JSON-serialized, as
   * required by the Telegram Bot API
   * @param {Object} obj Object; either 'form' or 'qs'
   * @private
   * @see https://core.telegram.org/bots/api#sendmessage
   */
  _fixReplyMarkup(obj) {
    const replyMarkup = obj.reply_markup;
    if (replyMarkup && typeof replyMarkup !== 'string') {
      obj.reply_markup = stringify(replyMarkup);
    }
  }

  /**
   * Fix 'entities' or 'caption_entities' or 'explanation_entities' parameter by making it JSON-serialized, as
   * required by the Telegram Bot API
   * @param {Object} obj Object;
   * @private
   * @see https://core.telegram.org/bots/api#sendmessage
   * @see https://core.telegram.org/bots/api#copymessage
   * @see https://core.telegram.org/bots/api#sendpoll
   */
  _fixEntitiesField(obj) {
    const entities = obj.entities;
    const captionEntities = obj.caption_entities;
    const explanationEntities = obj.explanation_entities;
    if (entities && typeof entities !== 'string') {
      obj.entities = stringify(entities);
    }

    if (captionEntities && typeof captionEntities !== 'string') {
      obj.caption_entities = stringify(captionEntities);
    }

    if (explanationEntities && typeof explanationEntities !== 'string') {
      obj.explanation_entities = stringify(explanationEntities);
    }
  }

  _fixAddFileThumbnail(options, opts) {
    if (options.thumb) {
      if (opts.formData === null) {
        opts.formData = {};
      }

      const attachName = 'photo';
      const [formData] = this._formatSendData(attachName, options.thumb.replace('attach://', ''));

      if (formData) {
        opts.formData[attachName] = formData[attachName];
        opts.qs.thumbnail = `attach://${attachName}`;
      }
    }
  }

  /**
   * Fix 'reply_parameters' parameter by making it JSON-serialized, as
   * required by the Telegram Bot API
   * @param {Object} obj Object; either 'form' or 'qs'
   * @private
   * @see https://core.telegram.org/bots/api#sendmessage
   */
  _fixReplyParameters(obj) {
    if (obj.hasOwnProperty('reply_parameters') && typeof obj.reply_parameters !== 'string') {
      obj.reply_parameters = stringify(obj.reply_parameters);
    }
  }

  /**
   * Make request against the API
   * @param  {String} _path API endpoint
   * @param  {Object} [options]
   * @private
   * @return {Promise}
   */
  _request(_path, options = {}) {
    if (!this.token) {
      return Promise.reject(new errors.FatalError('Telegram Bot Token not provided!'));
    }

    if (this.options.request) {
      Object.assign(options, this.options.request);
    }

    if (options.form) {
      this._fixReplyMarkup(options.form);
      this._fixEntitiesField(options.form);
      this._fixReplyParameters(options.form);
    }
    if (options.qs) {
      this._fixReplyMarkup(options.qs);
      this._fixReplyParameters(options.qs);
    }

    options.method = 'POST';
    options.url = this._buildURL(_path);
    options.simple = false;
    options.resolveWithFullResponse = true;
    options.forever = true;
    debug('HTTP request: %j', options);
    return request(options)
      .then(resp => {
        let data;
        try {
          data = resp.body = JSON.parse(resp.body);
        } catch (err) {
          throw new errors.ParseError(`Error parsing response: ${resp.body}`, resp);
        }

        if (data.ok) {
          return data.result;
        }

        throw new errors.TelegramError(`${data.error_code} ${data.description}`, resp);
      }).catch(error => {
        // TODO: why can't we do `error instanceof errors.BaseError`?
        if (error.response) throw error;
        throw new errors.FatalError(error);
      });
  }

  /**
   * Format data to be uploaded; handles file paths, streams and buffers
   * @param  {String} type
   * @param  {String|stream.Stream|Buffer} data
   * @param  {Object} fileOptions File options
   * @param  {String} [fileOptions.filename] File name
   * @param  {String} [fileOptions.contentType] Content type (i.e. MIME)
   * @return {Array} formatted
   * @return {Object} formatted[0] formData
   * @return {String} formatted[1] fileId
   * @throws Error if Buffer file type is not supported.
   * @see https://npmjs.com/package/file-type
   * @private
   */
  _formatSendData(type, data, fileOptions = {}) {
    const deprecationMessage =
      'See https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files' +
      ' for more information on how sending files has been improved and' +
      ' on how to disable this deprecation message altogether.';
    let filedata = data;
    let filename = fileOptions.filename;
    let contentType = fileOptions.contentType;

    if (data instanceof stream.Stream) {
      if (!filename && data.path) {
        // Will be 'null' if could not be parsed.
        // For example, 'data.path' === '/?id=123' from 'request("https://example.com/?id=123")'
        const url = URL.parse(path.basename(data.path.toString()));
        if (url.pathname) {
          filename = qs.unescape(url.pathname);
        }
      }
    } else if (Buffer.isBuffer(data)) {
      if (!filename && !process.env.NTBA_FIX_350) {
        deprecate(`Buffers will have their filenames default to "filename" instead of "data". ${deprecationMessage}`);
        filename = 'data';
      }
      if (!contentType) {
        const filetype = fileType(data);
        if (filetype) {
          contentType = filetype.mime;
          const ext = filetype.ext;
          if (ext && !process.env.NTBA_FIX_350) {
            filename = `${filename}.${ext}`;
          }
        } else if (!process.env.NTBA_FIX_350) {
          deprecate(`An error will no longer be thrown if file-type of buffer could not be detected. ${deprecationMessage}`);
          throw new errors.FatalError('Unsupported Buffer file-type');
        }
      }
    } else if (data) {
      if (this.options.filepath && fs.existsSync(data)) {
        filedata = fs.createReadStream(data);
        if (!filename) {
          filename = path.basename(data);
        }
      } else {
        return [null, data];
      }
    } else {
      return [null, data];
    }

    filename = filename || 'filename';
    contentType = contentType || mime.lookup(filename);
    if (process.env.NTBA_FIX_350) {
      contentType = contentType || 'application/octet-stream';
    } else {
      deprecate(`In the future, content-type of files you send will default to "application/octet-stream". ${deprecationMessage}`);
    }

    // TODO: Add missing file extension.

    return [{
      [type]: {
        value: filedata,
        options: {
          filename,
          contentType,
        },
      },
    }, null];
  }

  /**
   * Start polling.
   * Rejects returned promise if a WebHook is being used by this instance.
   * @param  {Object} [options]
   * @param  {Boolean} [options.restart=true] Consecutive calls to this method causes polling to be restarted
   * @return {Promise}
   */
  startPolling(options = {}) {
    if (this.hasOpenWebHook()) {
      return Promise.reject(new errors.FatalError('Polling and WebHook are mutually exclusive'));
    }
    options.restart = typeof options.restart === 'undefined' ? true : options.restart;
    if (!this._polling) {
      this._polling = new TelegramBotPolling(this);
    }
    return this._polling.start(options);
  }

  /**
   * Alias of `TelegramBot#startPolling()`. This is **deprecated**.
   * @param  {Object} [options]
   * @return {Promise}
   * @deprecated
   */
  initPolling() {
    deprecate('TelegramBot#initPolling() is deprecated. Use TelegramBot#startPolling() instead.');
    return this.startPolling();
  }

  /**
   * Stops polling after the last polling request resolves.
   * Multiple invocations do nothing if polling is already stopped.
   * Returning the promise of the last polling request is **deprecated**.
   * @param  {Object} [options] Options
   * @param  {Boolean} [options.cancel] Cancel current request
   * @param  {String} [options.reason] Reason for stopping polling
   * @return {Promise}
   */
  stopPolling(options) {
    if (!this._polling) {
      return Promise.resolve();
    }
    return this._polling.stop(options);
  }

  /**
   * Get link for file.
   * Use this method to get link for file for subsequent use.
   * Attention: link will be valid for 1 hour.
   *
   * This method is a sugar extension of the (getFile)[#getfilefileid] method,
   * which returns just path to file on remote server (you will have to manually build full uri after that).
   *
   * @param  {String} fileId  File identifier to get info about
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} Promise which will have  *fileURI* in resolve callback
   * @see https://core.telegram.org/bots/api#getfile
   */
  getFileLink(fileId, form = {}) {
    return this.getFile(fileId, form)
      .then(resp => `${this.options.baseApiUrl}/file/bot${this.token}/${resp.file_path}`);
  }

  /**
   * Return a readable stream for file.
   *
   * `fileStream.path` is the specified file ID i.e. `fileId`.
   * `fileStream` emits event `info` passing a single argument i.e.
   * `info` with the interface `{ uri }` where `uri` is the URI of the
   * file on Telegram servers.
   *
   * This method is a sugar extension of the [getFileLink](#TelegramBot+getFileLink) method,
   * which returns the full URI to the file on remote server.
   *
   * @param  {String} fileId File identifier to get info about
   * @param  {Object} [options] Additional Telegram query options
   * @return {stream.Readable} fileStream
   */
  getFileStream(fileId, form = {}) {
    const fileStream = new stream.PassThrough();
    fileStream.path = fileId;
    this.getFileLink(fileId, form)
      .then((fileURI) => {
        fileStream.emit('info', {
          uri: fileURI,
        });
        pump(streamedRequest(Object.assign({ uri: fileURI }, this.options.request)), fileStream);
      })
      .catch((error) => {
        fileStream.emit('error', error);
      });
    return fileStream;
  }

  /**
   * Downloads file in the specified folder.
   *
   * This method is a sugar extension of the [getFileStream](#TelegramBot+getFileStream) method,
   * which returns a readable file stream.
   *
   * @param  {String} fileId  File identifier to get info about
   * @param  {String} downloadDir Absolute path to the folder in which file will be saved
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} Promise, which will have *filePath* of downloaded file in resolve callback
   */
  downloadFile(fileId, downloadDir, form = {}) {
    let resolve;
    let reject;
    const promise = new Promise((a, b) => {
      resolve = a;
      reject = b;
    });
    const fileStream = this.getFileStream(fileId, form);
    fileStream.on('info', (info) => {
      const fileName = info.uri.slice(info.uri.lastIndexOf('/') + 1);
      // TODO: Ensure fileName doesn't contains slashes
      const filePath = path.join(downloadDir, fileName);
      pump(fileStream, fs.createWriteStream(filePath), (error) => {
        if (error) { return reject(error); }
        return resolve(filePath);
      });
    });
    fileStream.on('error', (err) => {
      reject(err);
    });
    return promise;
  }

  /**
   * Register a RegExp to test against an incomming text message.
   * @param  {RegExp}   regexpRexecuted with `exec`.
   * @param  {Function} callback     Callback will be called with 2 parameters,
   * the `msg` and the result of executing `regexp.exec` on message text.
   */
  onText(regexp, callback) {
    this._textRegexpCallbacks.push({ regexp, callback });
  }

  /**
   * Remove a listener registered with `onText()`.
   * @param  {RegExp} regexp RegExp used previously in `onText()`
   * @return {Object} deletedListener The removed reply listener if
   *   found. This object has `regexp` and `callback`
   *   properties. If not found, returns `null`.
   */
  removeTextListener(regexp) {
    const index = this._textRegexpCallbacks.findIndex((textListener) => {
      return String(textListener.regexp) === String(regexp);
    });
    if (index === -1) {
      return null;
    }
    return this._textRegexpCallbacks.splice(index, 1)[0];
  }

  /**
   * Remove all listeners registered with `onText()`.
   */
  clearTextListeners() {
    this._textRegexpCallbacks = [];
  }

  /**
   * Register a reply to wait for a message response.
   *
   * @param  {Number|String} chatId The chat id where the message cames from.
   * @param  {Number|String} messageId The message id to be replied.
   * @param  {Function} callback Callback will be called with the reply
   *  message.
   * @return {Number} id The ID of the inserted reply listener.
   */
  onReplyToMessage(chatId, messageId, callback) {
    const id = ++this._replyListenerId;
    this._replyListeners.push({
      id,
      chatId,
      messageId,
      callback
    });
    return id;
  }

  /**
   * Removes a reply that has been prev. registered for a message response.
   * @param   {Number} replyListenerId The ID of the reply listener.
   * @return  {Object} deletedListener The removed reply listener if
   *   found. This object has `id`, `chatId`, `messageId` and `callback`
   *   properties. If not found, returns `null`.
   */
  removeReplyListener(replyListenerId) {
    const index = this._replyListeners.findIndex((replyListener) => {
      return replyListener.id === replyListenerId;
    });
    if (index === -1) {
      return null;
    }
    return this._replyListeners.splice(index, 1)[0];
  }

  /**
   * Removes all replies that have been prev. registered for a message response.
   *
   * @return  {Array} deletedListeners An array of removed listeners.
   */
  clearReplyListeners() {
    this._replyListeners = [];
  }

  /**
   * Return true if polling. Otherwise, false.
   *
   * @return {Boolean}
   */
  isPolling() {
    return this._polling ? this._polling.isPolling() : false;
  }

  /**
   * Open webhook.
   * Multiple invocations do nothing if webhook is already open.
   * Rejects returned promise if Polling is being used by this instance.
   *
   * @return {Promise}
   */
  openWebHook() {
    if (this.isPolling()) {
      return Promise.reject(new errors.FatalError('WebHook and Polling are mutually exclusive'));
    }
    if (!this._webHook) {
      this._webHook = new TelegramBotWebHook(this);
    }
    return this._webHook.open();
  }

  /**
   * Close webhook after closing all current connections.
   * Multiple invocations do nothing if webhook is already closed.
   *
   * @return {Promise} Promise
   */
  closeWebHook() {
    if (!this._webHook) {
      return Promise.resolve();
    }
    return this._webHook.close();
  }

  /**
   * Return true if using webhook and it is open i.e. accepts connections.
   * Otherwise, false.
   *
   * @return {Boolean}
   */
  hasOpenWebHook() {
    return this._webHook ? this._webHook.isOpen() : false;
  }


  /**
   * Process an update; emitting the proper events and executing regexp
   * callbacks. This method is useful should you be using a different
   * way to fetch updates, other than those provided by TelegramBot.
   *
   * @param  {Object} update
   * @see https://core.telegram.org/bots/api#update
   */
  processUpdate(update) {
    debug('Process Update %j', update);
    const message = update.message;
    const editedMessage = update.edited_message;
    const channelPost = update.channel_post;
    const editedChannelPost = update.edited_channel_post;
    const businessConnection = update.business_connection;
    const businesssMessage = update.business_message;
    const editedBusinessMessage = update.edited_business_message;
    const deletedBusinessMessage = update.deleted_business_messages;
    const messageReaction = update.message_reaction;
    const messageReactionCount = update.message_reaction_count;
    const inlineQuery = update.inline_query;
    const chosenInlineResult = update.chosen_inline_result;
    const callbackQuery = update.callback_query;
    const shippingQuery = update.shipping_query;
    const preCheckoutQuery = update.pre_checkout_query;
    const poll = update.poll;
    const pollAnswer = update.poll_answer;
    const myChatMember = update.my_chat_member;
    const chatMember = update.chat_member;
    const chatJoinRequest = update.chat_join_request;
    const chatBoost = update.chat_boost;
    const removedChatBoost = update.removed_chat_boost;


    if (message) {
      debug('Process Update message %j', message);
      const metadata = {};
      metadata.type = TelegramBot.messageTypes.find((messageType) => {
        return message[messageType];
      });
      this.emit('message', message, metadata);
      if (metadata.type) {
        debug('Emitting %s: %j', metadata.type, message);
        this.emit(metadata.type, message, metadata);
      }
      if (message.text) {
        debug('Text message');
        this._textRegexpCallbacks.some(reg => {
          debug('Matching %s with %s', message.text, reg.regexp);

          if (!(reg.regexp instanceof RegExp)) {
            reg.regexp = new RegExp(reg.regexp);
          }

          const result = reg.regexp.exec(message.text);
          if (!result) {
            return false;
          }
          // reset index so we start at the beginning of the regex each time
          reg.regexp.lastIndex = 0;
          debug('Matches %s', reg.regexp);
          reg.callback(message, result);
          // returning truthy value exits .some
          return this.options.onlyFirstMatch;
        });
      }
      if (message.reply_to_message) {
        // Only callbacks waiting for this message
        this._replyListeners.forEach(reply => {
          // Message from the same chat
          if (reply.chatId === message.chat.id) {
            // Responding to that message
            if (reply.messageId === message.reply_to_message.message_id) {
              // Resolve the promise
              reply.callback(message);
            }
          }
        });
      }
    } else if (editedMessage) {
      debug('Process Update edited_message %j', editedMessage);
      this.emit('edited_message', editedMessage);
      if (editedMessage.text) {
        this.emit('edited_message_text', editedMessage);
      }
      if (editedMessage.caption) {
        this.emit('edited_message_caption', editedMessage);
      }
    } else if (channelPost) {
      debug('Process Update channel_post %j', channelPost);
      this.emit('channel_post', channelPost);
    } else if (editedChannelPost) {
      debug('Process Update edited_channel_post %j', editedChannelPost);
      this.emit('edited_channel_post', editedChannelPost);
      if (editedChannelPost.text) {
        this.emit('edited_channel_post_text', editedChannelPost);
      }
      if (editedChannelPost.caption) {
        this.emit('edited_channel_post_caption', editedChannelPost);
      }
    } else if (businessConnection) {
      debug('Process Update business_connection %j', businessConnection);
      this.emit('business_connection', businessConnection);
    } else if (businesssMessage) {
      debug('Process Update business_message %j', businesssMessage);
      this.emit('business_message', businesssMessage);
    } else if (editedBusinessMessage) {
      debug('Process Update edited_business_message %j', editedBusinessMessage);
      this.emit('edited_business_message', editedBusinessMessage);
    } else if (deletedBusinessMessage) {
      debug('Process Update deleted_business_messages %j', deletedBusinessMessage);
      this.emit('deleted_business_messages', deletedBusinessMessage);
    } else if (messageReaction) {
      debug('Process Update message_reaction %j', messageReaction);
      this.emit('message_reaction', messageReaction);
    } else if (messageReactionCount) {
      debug('Process Update message_reaction_count %j', messageReactionCount);
      this.emit('message_reaction_count', messageReactionCount);
    } else if (inlineQuery) {
      debug('Process Update inline_query %j', inlineQuery);
      this.emit('inline_query', inlineQuery);
    } else if (chosenInlineResult) {
      debug('Process Update chosen_inline_result %j', chosenInlineResult);
      this.emit('chosen_inline_result', chosenInlineResult);
    } else if (callbackQuery) {
      debug('Process Update callback_query %j', callbackQuery);
      this.emit('callback_query', callbackQuery);
    } else if (shippingQuery) {
      debug('Process Update shipping_query %j', shippingQuery);
      this.emit('shipping_query', shippingQuery);
    } else if (preCheckoutQuery) {
      debug('Process Update pre_checkout_query %j', preCheckoutQuery);
      this.emit('pre_checkout_query', preCheckoutQuery);
    } else if (poll) {
      debug('Process Update poll %j', poll);
      this.emit('poll', poll);
    } else if (pollAnswer) {
      debug('Process Update poll_answer %j', pollAnswer);
      this.emit('poll_answer', pollAnswer);
    } else if (chatMember) {
      debug('Process Update chat_member %j', chatMember);
      this.emit('chat_member', chatMember);
    } else if (myChatMember) {
      debug('Process Update my_chat_member %j', myChatMember);
      this.emit('my_chat_member', myChatMember);
    } else if (chatJoinRequest) {
      debug('Process Update my_chat_member %j', chatJoinRequest);
      this.emit('chat_join_request', chatJoinRequest);
    } else if (chatBoost) {
      debug('Process Update chat_boost %j', chatBoost);
      this.emit('chat_boost', chatBoost);
    } else if (removedChatBoost) {
      debug('Process Update removed_chat_boost %j', removedChatBoost);
      this.emit('removed_chat_boost', removedChatBoost);
    }
  }

  /** Start Telegram Bot API methods */

  /**
  * Use this method to receive incoming updates using long polling.
  * This method has an [older, compatible signature][getUpdates-v0.25.0]
  * that is being deprecated.
  *
  * @param  {Object} [options] Additional Telegram query options
  * @return {Promise}
  * @see https://core.telegram.org/bots/api#getupdates
  */
  getUpdates(form = {}) {
    /* The older method signature was getUpdates(timeout, limit, offset).
     * We need to ensure backwards-compatibility while maintaining
     * consistency of the method signatures throughout the library */
    if (typeof form !== 'object') {
      /* eslint-disable no-param-reassign, prefer-rest-params */
      deprecate('The method signature getUpdates(timeout, limit, offset) has been deprecated since v0.25.0');
      form = {
        timeout: arguments[0],
        limit: arguments[1],
        offset: arguments[2],
      };
      /* eslint-enable no-param-reassign, prefer-rest-params */
    }

    return this._request('getUpdates', { form });
  }

  /**
   * Specify an url to receive incoming updates via an outgoing webHook.
   * This method has an [older, compatible signature][setWebHook-v0.25.0]
   * that is being deprecated.
   *
   * @param  {String} url URL where Telegram will make HTTP Post. Leave empty to
   * delete webHook.
   * @param  {Object} [options] Additional Telegram query options
   * @param  {String|stream.Stream} [options.certificate] PEM certificate key (public).
   * @param  {String} [options.secret_token] Optional secret token to be sent in a header `X-Telegram-Bot-Api-Secret-Token` in every webhook request.
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise}
   * @see https://core.telegram.org/bots/api#setwebhook
   * @see https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files
   */
  setWebHook(url, options = {}, fileOptions = {}) {
    /* The older method signature was setWebHook(url, cert).
     * We need to ensure backwards-compatibility while maintaining
     * consistency of the method signatures throughout the library */
    let cert;
    // Note: 'options' could be an object, if a stream was provided (in place of 'cert')
    if (typeof options !== 'object' || options instanceof stream.Stream) {
      deprecate('The method signature setWebHook(url, cert) has been deprecated since v0.25.0');
      cert = options;
      options = {}; // eslint-disable-line no-param-reassign
    } else {
      cert = options.certificate;
    }

    const opts = {
      qs: options,
    };
    opts.qs.url = url;

    if (cert) {
      try {
        const sendData = this._formatSendData('certificate', cert, fileOptions);
        opts.formData = sendData[0];
        opts.qs.certificate = sendData[1];
      } catch (ex) {
        return Promise.reject(ex);
      }
    }

    return this._request('setWebHook', opts);
  }

  /**
   * Use this method to remove webhook integration if you decide to
   * switch back to getUpdates. Returns True on success.
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise}
   * @see https://core.telegram.org/bots/api#deletewebhook
   */
  deleteWebHook(form = {}) {
    return this._request('deleteWebhook', { form });
  }

  /**
   * Use this method to get current webhook status.
   * On success, returns a [WebhookInfo](https://core.telegram.org/bots/api#webhookinfo) object.
   * If the bot is using getUpdates, will return an object with the
   * url field empty.
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise}
   * @see https://core.telegram.org/bots/api#getwebhookinfo
   */
  getWebHookInfo(form = {}) {
    return this._request('getWebhookInfo', { form });
  }

  /**
   * A simple method for testing your bot's authentication token. Requires no parameters.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} basic information about the bot in form of a [User](https://core.telegram.org/bots/api#user) object.
   * @see https://core.telegram.org/bots/api#getme
   */
  getMe(form = {}) {
    return this._request('getMe', { form });
  }

  /**
   * This method log out your bot from the cloud Bot API server before launching the bot locally.
   * You must log out the bot before running it locally, otherwise there is no guarantee that the bot will receive updates.
   * After a successful call, you will not be able to log in again using the same token for 10 minutes.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise}  True on success
   * @see https://core.telegram.org/bots/api#logout
   */
  logOut(form = {}) {
    return this._request('logOut', { form });
  }

  /**
   * This method close the bot instance before moving it from one local server to another.
   * This method will return error 429 in the first 10 minutes after the bot is launched.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise}  True on success
   * @see https://core.telegram.org/bots/api#close
   */
  close(form = {}) {
    return this._request('close', { form });
  }

  /**
   * Send text message.
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String} text Text of the message to be sent
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned
   * @see https://core.telegram.org/bots/api#sendmessage
   */
  sendMessage(chatId, text, form = {}) {
    form.chat_id = chatId;
    form.text = text;
    return this._request('sendMessage', { form });
  }

  /**
   * Forward messages of any kind.
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * or username of the target channel (in the format `@channelusername`)
   * @param  {Number|String} fromChatId Unique identifier for the chat where the
   * original message was sent (or channel username in the format `@channelusername`)
   * @param  {Number|String} messageId  Unique message identifier in the chat specified in fromChatId
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise}
   * @see https://core.telegram.org/bots/api#forwardmessage
   */
  forwardMessage(chatId, fromChatId, messageId, form = {}) {
    form.chat_id = chatId;
    form.from_chat_id = fromChatId;
    form.message_id = messageId;
    return this._request('forwardMessage', { form });
  }

  /**
   * Use this method to forward multiple messages of any kind.
   * If some of the specified messages can't be found or forwarded, they are skipped.
   *
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * or username of the target channel (in the format `@channelusername`)
   * @param  {Number|String} fromChatId Unique identifier for the chat where the
   * original message was sent (or channel username in the format `@channelusername`)
   * @param  {Array<Number|String>} messageIds Identifiers of 1-100 messages in the chat from_chat_id to forward.
   * The identifiers must be specified in a strictly increasing order.
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} An array of MessageId of the sent messages on success
   * @see https://core.telegram.org/bots/api#forwardmessages
   */
  forwardMessages(chatId, fromChatId, messageIds, form = {}) {
    form.chat_id = chatId;
    form.from_chat_id = fromChatId;
    form.message_ids = messageIds;
    return this._request('forwardMessages', { form });
  }

  /**
   * Copy messages of any kind. **Service messages and invoice messages can't be copied.**
   * The method is analogous to the method forwardMessages, but the copied message doesn't
   * have a link to the original message.
   * Returns the MessageId of the sent message on success.
   * @param  {Number|String} chatId     Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Number|String} fromChatId Unique identifier for the chat where the
   * original message was sent
   * @param  {Number|String} messageId  Unique message identifier
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} The [MessageId](https://core.telegram.org/bots/api#messageid) of the sent message on success
   * @see https://core.telegram.org/bots/api#copymessage
   */
  copyMessage(chatId, fromChatId, messageId, form = {}) {
    form.chat_id = chatId;
    form.from_chat_id = fromChatId;
    form.message_id = messageId;
    return this._request('copyMessage', { form });
  }

  /**
   * Use this method to copy messages of any kind. If some of the specified messages can't be found or copied, they are skipped.
   * Service messages, giveaway messages, giveaway winners messages, and invoice messages can't be copied.
   * Returns the MessageId of the sent message on success.
   * @param  {Number|String} chatId Unique identifier for the target chat
   * @param  {Number|String} fromChatId Unique identifier for the chat where the
   * original message was sent
   * @param  {Array} messageIds  Identifiers of 1-100 messages in the chat from_chat_id to copy.
   * The identifiers must be specified in a strictly increasing order.
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} An array of MessageId of the sent messages
   * @see https://core.telegram.org/bots/api#copymessages
   */
  copyMessages(chatId, fromChatId, messageIds, form = {}) {
    form.chat_id = chatId;
    form.from_chat_id = fromChatId;
    form.message_ids = stringify(messageIds);
    return this._request('copyMessages', { form });
  }

  /**
   * Send photo
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String|stream.Stream|Buffer} photo A file path or a Stream. Can
   * also be a `file_id` previously uploaded
   * @param  {Object} [options] Additional Telegram query options
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned
   * @see https://core.telegram.org/bots/api#sendphoto
   * @see https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files
   */
  sendPhoto(chatId, photo, options = {}, fileOptions = {}) {
    const opts = {
      qs: options,
    };
    opts.qs.chat_id = chatId;
    try {
      const sendData = this._formatSendData('photo', photo, fileOptions);
      opts.formData = sendData[0];
      opts.qs.photo = sendData[1];
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('sendPhoto', opts);
  }

  /**
  * Send audio
  *
  * **Your audio must be in the .MP3 or .M4A format.**
  *
  * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
  * @param  {String|stream.Stream|Buffer} audio A file path, Stream or Buffer.
  * Can also be a `file_id` previously uploaded.
  * @param  {Object} [options] Additional Telegram query options
  * @param  {Object} [fileOptions] Optional file related meta-data
  * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned
  * @see https://core.telegram.org/bots/api#sendaudio
  * @see https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files
  */
  sendAudio(chatId, audio, options = {}, fileOptions = {}) {
    const opts = {
      qs: options
    };

    opts.qs.chat_id = chatId;

    try {
      const sendData = this._formatSendData('audio', audio, fileOptions);
      opts.formData = sendData[0];
      opts.qs.audio = sendData[1];
      this._fixAddFileThumbnail(options, opts);
    } catch (ex) {
      return Promise.reject(ex);
    }

    return this._request('sendAudio', opts);
  }

  /**
  * Send Document
  * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
  * @param  {String|stream.Stream|Buffer} doc A file path, Stream or Buffer.
  * Can also be a `file_id` previously uploaded.
  * @param  {Object} [options] Additional Telegram query options
  * @param  {Object} [fileOptions] Optional file related meta-data
  * @return {Promise}  On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned
  * @see https://core.telegram.org/bots/api#sendDocument
  * @see https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files
  */
  sendDocument(chatId, doc, options = {}, fileOptions = {}) {
    const opts = {
      qs: options
    };
    opts.qs.chat_id = chatId;
    try {
      const sendData = this._formatSendData('document', doc, fileOptions);
      opts.formData = sendData[0];
      opts.qs.document = sendData[1];
      this._fixAddFileThumbnail(options, opts);
    } catch (ex) {
      return Promise.reject(ex);
    }

    return this._request('sendDocument', opts);
  }

  /**
   * Use this method to send video files, **Telegram clients support mp4 videos** (other formats may be sent as Document).
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String|stream.Stream|Buffer} video A file path or Stream.
   * Can also be a `file_id` previously uploaded.
   * @param  {Object} [options] Additional Telegram query options
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned
   * @see https://core.telegram.org/bots/api#sendvideo
   * @see https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files
   */
  sendVideo(chatId, video, options = {}, fileOptions = {}) {
    const opts = {
      qs: options
    };
    opts.qs.chat_id = chatId;
    try {
      const sendData = this._formatSendData('video', video, fileOptions);
      opts.formData = sendData[0];
      opts.qs.video = sendData[1];
      this._fixAddFileThumbnail(options, opts);
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('sendVideo', opts);
  }

  /**
   * Use this method to send animation files (GIF or H.264/MPEG-4 AVC video without sound).
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String|stream.Stream|Buffer} animation A file path, Stream or Buffer.
   * Can also be a `file_id` previously uploaded.
   * @param  {Object} [options] Additional Telegram query options
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned
   * @see https://core.telegram.org/bots/api#sendanimation
   * @see https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files
   */
  sendAnimation(chatId, animation, options = {}, fileOptions = {}) {
    const opts = {
      qs: options
    };
    opts.qs.chat_id = chatId;
    try {
      const sendData = this._formatSendData('animation', animation, fileOptions);
      opts.formData = sendData[0];
      opts.qs.animation = sendData[1];
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('sendAnimation', opts);
  }

  /**
   * Send voice
   *
   * **Your audio must be in an .OGG file encoded with OPUS**, or in .MP3 format, or in .M4A format (other formats may be sent as Audio or Document)
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String|stream.Stream|Buffer} voice A file path, Stream or Buffer.
   * Can also be a `file_id` previously uploaded.
   * @param  {Object} [options] Additional Telegram query options
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned
   * @see https://core.telegram.org/bots/api#sendvoice
   * @see https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files
   */
  sendVoice(chatId, voice, options = {}, fileOptions = {}) {
    const opts = {
      qs: options
    };
    opts.qs.chat_id = chatId;
    try {
      const sendData = this._formatSendData('voice', voice, fileOptions);
      opts.formData = sendData[0];
      opts.qs.voice = sendData[1];
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('sendVoice', opts);
  }

  /**
   * Use this method to send video messages
   * Telegram clients support **rounded square MPEG4 videos** of up to 1 minute long.
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String|stream.Stream|Buffer} videoNote A file path or Stream.
   * Can also be a `file_id` previously uploaded.
   * @param  {Object} [options] Additional Telegram query options
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned
   * @info The length parameter is actually optional. However, the API (at time of writing) requires you to always provide it until it is fixed.
   * @see https://core.telegram.org/bots/api#sendvideonote
   * @see https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files
  */
  sendVideoNote(chatId, videoNote, options = {}, fileOptions = {}) {
    const opts = {
      qs: options
    };
    opts.qs.chat_id = chatId;
    try {
      const sendData = this._formatSendData('video_note', videoNote, fileOptions);
      opts.formData = sendData[0];
      opts.qs.video_note = sendData[1];
      this._fixAddFileThumbnail(options, opts);
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('sendVideoNote', opts);
  }

  /**
   * Use this method to send a group of photos or videos as an album.
   *
   * **Documents and audio files can be only grouped in an album with messages of the same type**
   *
   * If you wish to [specify file options](https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files),
   * add a `fileOptions` property to the target input in `media`.
   *
   * @param  {String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Array} media A JSON-serialized array describing photos and videos to be sent, must include 2–10 items
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, an array of the sent [Messages](https://core.telegram.org/bots/api#message)
   * is returned.
   * @see https://core.telegram.org/bots/api#sendmediagroup
   * @see https://github.com/yagop/node-telegram-bot-api/blob/master/doc/usage.md#sending-files
   */
  sendMediaGroup(chatId, media, options = {}) {
    const opts = {
      qs: options,
    };
    opts.qs.chat_id = chatId;

    opts.formData = {};
    const inputMedia = [];
    let index = 0;
    for (const input of media) {
      const payload = Object.assign({}, input);
      delete payload.media;
      delete payload.fileOptions;
      try {
        const attachName = String(index);
        const [formData, fileId] = this._formatSendData(attachName, input.media, input.fileOptions);
        if (formData) {
          opts.formData[attachName] = formData[attachName];
          payload.media = `attach://${attachName}`;
        } else {
          payload.media = fileId;
        }
      } catch (ex) {
        return Promise.reject(ex);
      }
      inputMedia.push(payload);
      index++;
    }
    opts.qs.media = stringify(inputMedia);

    return this._request('sendMediaGroup', opts);
  }


  /**
   * Send location.
   * Use this method to send point on the map.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Float} latitude Latitude of location
   * @param  {Float} longitude Longitude of location
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned
   * @see https://core.telegram.org/bots/api#sendlocation
   */
  sendLocation(chatId, latitude, longitude, form = {}) {
    form.chat_id = chatId;
    form.latitude = latitude;
    form.longitude = longitude;
    return this._request('sendLocation', { form });
  }

  /**
   * Use this method to edit live location messages sent by
   * the bot or via the bot (for inline bots).
   *
   *  A location **can be edited until its live_period expires or editing is explicitly disabled by a call to [stopMessageLiveLocation](https://core.telegram.org/bots/api#stopmessagelivelocation)**
   *
   * Note that you must provide one of chat_id, message_id, or
   * inline_message_id in your request.
   *
   * @param  {Float} latitude Latitude of location
   * @param  {Float} longitude Longitude of location
   * @param  {Object} [options] Additional Telegram query options (provide either one of chat_id, message_id, or inline_message_id here)
   * @return {Promise} On success, if the edited message is not an inline message, the edited [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned.
   * @see https://core.telegram.org/bots/api#editmessagelivelocation
   */
  editMessageLiveLocation(latitude, longitude, form = {}) {
    form.latitude = latitude;
    form.longitude = longitude;
    return this._request('editMessageLiveLocation', { form });
  }

  /**
   * Use this method to stop updating a live location message sent by
   * the bot or via the bot (for inline bots) before live_period expires.
   *
   * Note that you must provide one of chat_id, message_id, or
   * inline_message_id in your request.
   *
   * @param  {Object} [options] Additional Telegram query options (provide either one of chat_id, message_id, or inline_message_id here)
   * @return {Promise} On success, if the edited message is not an inline message, the edited [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned.
   * @see https://core.telegram.org/bots/api#stopmessagelivelocation
   */
  stopMessageLiveLocation(form = {}) {
    return this._request('stopMessageLiveLocation', { form });
  }

  /**
   * Send venue.
   * Use this method to send information about a venue.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Float} latitude Latitude of location
   * @param  {Float} longitude Longitude of location
   * @param  {String} title Name of the venue
   * @param  {String} address Address of the venue
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned.
   * @see https://core.telegram.org/bots/api#sendvenue
   */
  sendVenue(chatId, latitude, longitude, title, address, form = {}) {
    form.chat_id = chatId;
    form.latitude = latitude;
    form.longitude = longitude;
    form.title = title;
    form.address = address;
    return this._request('sendVenue', { form });
  }

  /**
   * Send contact.
   * Use this method to send phone contacts.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String} phoneNumber Contact's phone number
   * @param  {String} firstName Contact's first name
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned
   * @see https://core.telegram.org/bots/api#sendcontact
   */
  sendContact(chatId, phoneNumber, firstName, form = {}) {
    form.chat_id = chatId;
    form.phone_number = phoneNumber;
    form.first_name = firstName;
    return this._request('sendContact', { form });
  }

  /**
   * Send poll.
   * Use this method to send a native poll.
   *
   * @param  {Number|String} chatId  Unique identifier for the group/channel
   * @param  {String} question Poll question, 1-300 characters
   * @param  {Array} pollOptions Poll options, between 2-10 options (only 1-100 characters each)
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned
   * @see https://core.telegram.org/bots/api#sendpoll
   */
  sendPoll(chatId, question, pollOptions, form = {}) {
    form.chat_id = chatId;
    form.question = question;
    form.options = stringify(pollOptions);
    return this._request('sendPoll', { form });
  }

  /**
   * Send Dice
   * Use this method to send an animated emoji that will display a random value.
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise}  On success, the sent [Message](https://core.telegram.org/bots/api#message) object is returned
   * @see https://core.telegram.org/bots/api#senddice
   */
  sendDice(chatId, options = {}) {
    const opts = {
      qs: options,
    };
    opts.qs.chat_id = chatId;
    try {
      const sendData = this._formatSendData('dice');
      opts.formData = sendData[0];
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('sendDice', opts);
  }


  /**
   * Send chat action.
   *
   * Use this method when you need to tell the user that something is happening on the bot's side.
   * **The status is set for 5 seconds or less** (when a message arrives from your bot, Telegram clients clear its typing status).
   *
   *  Action `typing` for [text messages](https://core.telegram.org/bots/api#sendmessage),
   * `upload_photo` for [photos](https://core.telegram.org/bots/api#sendphoto), `record_video` or `upload_video` for [videos](https://core.telegram.org/bots/api#sendvideo),
   * `record_voice` or `upload_voice` for [voice notes](https://core.telegram.org/bots/api#sendvoice), `upload_document` for [general files](https://core.telegram.org/bots/api#senddocument),
   * `choose_sticker` for [stickers](https://core.telegram.org/bots/api#sendsticker), `find_location` for [location data](https://core.telegram.org/bots/api#sendlocation),
   * `record_video_note` or `upload_video_note` for [video notes](https://core.telegram.org/bots/api#sendvideonote).
   *
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String} action Type of action to broadcast.
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#sendchataction
   */
  sendChatAction(chatId, action, form = {}) {
    form.chat_id = chatId;
    form.action = action;
    return this._request('sendChatAction', { form });
  }

  /**
   * Use this method to change the chosen reactions on a message.
   * - Service messages can't be reacted to.
   * - Automatically forwarded messages from a channel to its discussion group have the same available reactions as messages in the channel.
   * - In albums, bots must react to the first message.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format @channelusername)
   * @param  {Number} messageId  Unique identifier of the target message
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise<Boolean>} True on success
   * @see https://core.telegram.org/bots/api#setmessagereaction
   */
  setMessageReaction(chatId, messageId, form = {}) {
    form.chat_id = chatId;
    form.message_id = messageId;
    if (form.reaction) {
      form.reaction = stringify(form.reaction);
    }
    return this._request('setMessageReaction', { form });
  }

  /**
   * Use this method to get a list of profile pictures for a user.
   * Returns a [UserProfilePhotos](https://core.telegram.org/bots/api#userprofilephotos) object.
   * This method has an [older, compatible signature][getUserProfilePhotos-v0.25.0]
   * that is being deprecated.
   *
   * @param  {Number} userId  Unique identifier of the target user
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise}  Returns a [UserProfilePhotos](https://core.telegram.org/bots/api#userprofilephotos) object
   * @see https://core.telegram.org/bots/api#getuserprofilephotos
   */
  getUserProfilePhotos(userId, form = {}) {
    /* The older method signature was getUserProfilePhotos(userId, offset, limit).
     * We need to ensure backwards-compatibility while maintaining
     * consistency of the method signatures throughout the library */
    if (typeof form !== 'object') {
      /* eslint-disable no-param-reassign, prefer-rest-params */
      deprecate('The method signature getUserProfilePhotos(userId, offset, limit) has been deprecated since v0.25.0');
      form = {
        offset: arguments[1],
        limit: arguments[2],
      };
      /* eslint-enable no-param-reassign, prefer-rest-params */
    }
    form.user_id = userId;
    return this._request('getUserProfilePhotos', { form });
  }

  /**
   * Get file.
   * Use this method to get basic info about a file and prepare it for downloading.
   *
   * Attention: **link will be valid for 1 hour.**
   *
   * @param  {String} fileId  File identifier to get info about
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, a [File](https://core.telegram.org/bots/api#file) object is returned
   * @see https://core.telegram.org/bots/api#getfile
   */
  getFile(fileId, form = {}) {
    form.file_id = fileId;
    return this._request('getFile', { form });
  }

  /**
    * Use this method to ban a user in a group, a supergroup or a channel.
    * In the case of supergroups and channels, the user will not be able to
    * return to the chat on their own using invite links, etc., unless unbanned first..
    *
    * The **bot must be an administrator in the group, supergroup or a channel** for this to work.
    *
    *
    * @param  {Number|String} chatId   Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
    * @param  {Number} userId  Unique identifier of the target user
    * @param  {Object} [options] Additional Telegram query options
    * @return {Promise} True on success.
    * @see https://core.telegram.org/bots/api#banchatmember
    */
  banChatMember(chatId, userId, form = {}) {
    form.chat_id = chatId;
    form.user_id = userId;
    return this._request('banChatMember', { form });
  }

  /**
  * Use this method to unban a previously kicked user in a supergroup.
  * The user will not return to the group automatically, but will be
  * able to join via link, etc.
  *
  * The **bot must be an administrator** in the supergroup or channel for this to work.
  *
  * **By default**, this method guarantees that after the call the user is not a member of the chat, but will be able to join it.
  * So **if the user is a member of the chat they will also be removed from the chat**. If you don't want this, use the parameter *only_if_banned*
  *
  * @param  {Number|String} chatId   Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
  * @param  {Number} userId  Unique identifier of the target user
  * @param  {Object} [options] Additional Telegram query options
  * @return {Promise} True on success
  * @see https://core.telegram.org/bots/api#unbanchatmember
  */
  unbanChatMember(chatId, userId, form = {}) {
    form.chat_id = chatId;
    form.user_id = userId;
    return this._request('unbanChatMember', { form });
  }

  /**
  * Use this method to restrict a user in a supergroup.
  * The bot **must be an administrator in the supergroup** for this to work
  * and must have the appropriate admin rights. Pass True for all boolean parameters
  * to lift restrictions from a user. Returns True on success.
  *
  * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
  * @param  {Number} userId Unique identifier of the target user
  * @param  {Object} [options] Additional Telegram query options
  * @return {Promise} True on success
  * @see https://core.telegram.org/bots/api#restrictchatmember
  */
  restrictChatMember(chatId, userId, form = {}) {
    form.chat_id = chatId;
    form.user_id = userId;
    return this._request('restrictChatMember', { form });
  }

  /**
   * Use this method to promote or demote a user in a supergroup or a channel.
   * The bot **must be an administrator** in the chat for this to work
   * and must have the appropriate admin rights. Pass False for all boolean parameters to demote a user.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Number} userId
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success.
   * @see https://core.telegram.org/bots/api#promotechatmember
   */
  promoteChatMember(chatId, userId, form = {}) {
    form.chat_id = chatId;
    form.user_id = userId;
    return this._request('promoteChatMember', { form });
  }

  /**
   * Use this method to set a custom title for an administrator in a supergroup promoted by the bot.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Number} userId Unique identifier of the target user
   * @param  {String} customTitle New custom title for the administrator; 0-16 characters, emoji are not allowed
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setchatadministratorcustomtitle
   */
  setChatAdministratorCustomTitle(chatId, userId, customTitle, form = {}) {
    form.chat_id = chatId;
    form.user_id = userId;
    form.custom_title = customTitle;
    return this._request('setChatAdministratorCustomTitle', { form });
  }


  /**
   * Use this method to ban a channel chat in a supergroup or a channel.
   *
   * Until the chat is [unbanned](https://core.telegram.org/bots/api#unbanchatsenderchat), the owner of the banned chat won't be able to send messages on behalf of any of their channels.
   * The bot **must be an administrator in the supergroup or channel** for this to work and must have the appropriate administrator rights
   *
   * @param  {Number|String} chatId   Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Number} senderChatId  Unique identifier of the target user
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success.
   * @see https://core.telegram.org/bots/api#banchatsenderchat
   */
  banChatSenderChat(chatId, senderChatId, form = {}) {
    form.chat_id = chatId;
    form.sender_chat_id = senderChatId;
    return this._request('banChatSenderChat', { form });
  }

  /**
  * Use this method to unban a previously banned channel chat in a supergroup or channel.
  *
  * The bot **must be an administrator** for this to work and must have the appropriate administrator rights.
  *
  * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
  * @param  {Number} senderChatId Unique identifier of the target user
  * @param  {Object} [options] Additional Telegram query options
  * @return {Promise} True on success
  * @see https://core.telegram.org/bots/api#unbanchatsenderchat
  */
  unbanChatSenderChat(chatId, senderChatId, form = {}) {
    form.chat_id = chatId;
    form.sender_chat_id = senderChatId;
    return this._request('unbanChatSenderChat', { form });
  }

  /**
   * Use this method to set default chat permissions for all members.
   *
   * The bot **must be an administrator in the group or a supergroup** for this to
   * work and **must have the `can_restrict_members` admin rights.**
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Array} chatPermissions New default chat permissions
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setchatpermissions
   */
  setChatPermissions(chatId, chatPermissions, form = {}) {
    form.chat_id = chatId;
    form.permissions = stringify(chatPermissions);
    return this._request('setChatPermissions', { form });
  }

  /**
   * Use this method to generate a new primary invite link for a chat. **Any previously generated primary link is revoked**.
   *
   * The bot **must be an administrator in the chat** for this to work and must have the appropriate administrator rights.
   *
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} Exported invite link as String on success.
   * @see https://core.telegram.org/bots/api#exportchatinvitelink
   */
  exportChatInviteLink(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('exportChatInviteLink', { form });
  }

  /**
   * Use this method to create an additional invite link for a chat.
   *
   * The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.
   *
   * The link generated with this method can be revoked using the method [revokeChatInviteLink](https://core.telegram.org/bots/api#revokechatinvitelink)
   *
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Object} [options] Additional Telegram query options
   * @return {Object} The new invite link as [ChatInviteLink](https://core.telegram.org/bots/api#chatinvitelink) object
   * @see https://core.telegram.org/bots/api#createchatinvitelink
   */
  createChatInviteLink(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('createChatInviteLink', { form });
  }

  /**
   * Use this method to edit a non-primary invite link created by the bot.
   *
   * The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.
   *
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String} inviteLink Text with the invite link to edit
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} The edited invite link as a [ChatInviteLink](https://core.telegram.org/bots/api#chatinvitelink) object
   * @see https://core.telegram.org/bots/api#editchatinvitelink
   */
  editChatInviteLink(chatId, inviteLink, form = {}) {
    form.chat_id = chatId;
    form.invite_link = inviteLink;
    return this._request('editChatInviteLink', { form });
  }

  /**
   * Use this method to create a subscription invite link for a channel chat.
   *
   * The bot must have the can_invite_users administrator rights
   *
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Number} subscriptionPeriod The number of seconds the subscription will be active for before the next payment. Currently, it must always be 2592000 (30 days)
   * @param  {Number} subscriptionPrice The amount of Telegram Stars a user must pay initially and after each subsequent subscription period to be a member of the chat (1-2500)
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} The new invite link as a [ChatInviteLink](https://core.telegram.org/bots/api#chatinvitelink) object
   * @see https://core.telegram.org/bots/api#createchatsubscriptioninvitelink
   */
  createChatSubscriptionInviteLink(chatId, subscriptionPeriod, subscriptionPrice, form = {}) {
    form.chat_id = chatId;
    form.subscription_period = subscriptionPeriod;
    form.subscription_price = subscriptionPrice;
    return this._request('createChatSubscriptionInviteLink', { form });
  }

  /**
   * Use this method to edit a subscription invite link created by the bot.
   *
   * The bot must have the can_invite_users administrator rights
   *
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String} inviteLink The invite link to edit
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} The new invite link as a [ChatInviteLink](https://core.telegram.org/bots/api#chatinvitelink) object
   * @see https://core.telegram.org/bots/api#editchatsubscriptioninvitelink
   */
  editChatSubscriptionInviteLink(chatId, inviteLink, form = {}) {
    form.chat_id = chatId;
    form.invite_link = inviteLink;
    return this._request('editChatSubscriptionInviteLink', { form });
  }

  /**
   * Use this method to revoke an invite link created by the bot.
   * Note: If the primary link is revoked, a new link is automatically generated
   *
   * The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.
   *
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String} inviteLink The invite link to revoke
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} The revoked invite link as [ChatInviteLink](https://core.telegram.org/bots/api#chatinvitelink) object
   * @see https://core.telegram.org/bots/api#revokechatinvitelink
   */
  revokeChatInviteLink(chatId, inviteLink, form = {}) {
    form.chat_id = chatId;
    form.invite_link = inviteLink;
    return this._request('revokeChatInviteLink', { form });
  }

  /**
   * Use this method to approve a chat join request.
   *
   * The bot **must be an administrator in the chat** for this to work and **must have the `can_invite_users` administrator right.**
   *
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Number} userId  Unique identifier of the target user
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#approvechatjoinrequest
   */
  approveChatJoinRequest(chatId, userId, form = {}) {
    form.chat_id = chatId;
    form.user_id = userId;
    return this._request('approveChatJoinRequest', { form });
  }

  /**
   * Use this method to decline a chat join request.
   *
   * The bot **must be an administrator in the chat** for this to work and **must have the `can_invite_users` administrator right**.
   *
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Number} userId  Unique identifier of the target user
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#declinechatjoinrequest
   */
  declineChatJoinRequest(chatId, userId, form = {}) {
    form.chat_id = chatId;
    form.user_id = userId;
    return this._request('declineChatJoinRequest', { form });
  }

  /**
   * Use this method to set a new profile photo for the chat. **Photos can't be changed for private chats**.
   *
   * The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {stream.Stream|Buffer} photo A file path or a Stream.
   * @param  {Object} [options] Additional Telegram query options
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setchatphoto
   */
  setChatPhoto(chatId, photo, options = {}, fileOptions = {}) {
    const opts = {
      qs: options,
    };
    opts.qs.chat_id = chatId;
    try {
      const sendData = this._formatSendData('photo', photo, fileOptions);
      opts.formData = sendData[0];
      opts.qs.photo = sendData[1];
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('setChatPhoto', opts);
  }

  /**
  * Use this method to delete a chat photo. **Photos can't be changed for private chats**.
  *
  * The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.
  *
  * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
  * @param  {Object} [options] Additional Telegram query options
  * @return {Promise} True on success
  * @see https://core.telegram.org/bots/api#deletechatphoto
  */
  deleteChatPhoto(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('deleteChatPhoto', { form });
  }

  /**
   * Use this method to change the title of a chat. **Titles can't be changed for private chats**.
   *
   * The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String} title New chat title, 1-255 characters
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setchattitle
   */
  setChatTitle(chatId, title, form = {}) {
    form.chat_id = chatId;
    form.title = title;
    return this._request('setChatTitle', { form });
  }

  /**
   * Use this method to change the description of a group, a supergroup or a channel.
   *
   * The bot **must be an administrator in the chat** for this to work and must have the appropriate admin rights.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String} description New chat title, 0-255 characters
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setchatdescription
   */
  setChatDescription(chatId, description, form = {}) {
    form.chat_id = chatId;
    form.description = description;
    return this._request('setChatDescription', { form });
  }

  /**
   * Use this method to pin a message in a supergroup.
   *
   * If the chat is not a private chat, the **bot must be an administrator in the chat** for this to work and must have the `can_pin_messages` administrator
   * right in a supergroup or `can_edit_messages` administrator right in a channel.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Number} messageId Identifier of a message to pin
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#pinchatmessage
   */
  pinChatMessage(chatId, messageId, form = {}) {
    form.chat_id = chatId;
    form.message_id = messageId;
    return this._request('pinChatMessage', { form });
  }

  /**
   * Use this method to remove a message from the list of pinned messages in a chat
   *
   * If the chat is not a private chat, the **bot must be an administrator in the chat** for this to work and must have the `can_pin_messages` administrator
   * right in a supergroup or `can_edit_messages` administrator right in a channel.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#unpinchatmessage
   */
  unpinChatMessage(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('unpinChatMessage', { form });
  }

  /**
  * Use this method to clear the list of pinned messages in a chat.
  *
  * If the chat is not a private chat, the **bot must be an administrator in the chat** for this to work and must have the `can_pin_messages` administrator
  * right in a supergroup or `can_edit_messages` administrator right in a channel.
  *
  * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
  * @param  {Object} [options] Additional Telegram query options
  * @return {Promise} True on success
  * @see https://core.telegram.org/bots/api#unpinallchatmessages
  */
  unpinAllChatMessages(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('unpinAllChatMessages', { form });
  }

  /**
   * Use this method for your bot to leave a group, supergroup or channel
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#leavechat
   */
  leaveChat(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('leaveChat', { form });
  }

  /**
   * Use this method to get up to date information about the chat
   * (current name of the user for one-on-one conversations, current
   * username of a user, group or channel, etc.).
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`) or channel
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} [ChatFullInfo](https://core.telegram.org/bots/api#chatfullinfo) object on success
   * @see https://core.telegram.org/bots/api#getchat
   */
  getChat(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('getChat', { form });
  }

  /**
   * Use this method to get a list of administrators in a chat
   *
   * @param  {Number|String} chatId  Unique identifier for the target group or username of the target supergroup
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, returns an Array of [ChatMember](https://core.telegram.org/bots/api#chatmember) objects that contains information about all chat administrators except other bots.
   * If the chat is a group or a supergroup and no administrators were appointed, only the creator will be returned
   * @see https://core.telegram.org/bots/api#getchatadministrators
   */
  getChatAdministrators(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('getChatAdministrators', { form });
  }

  /**
  * Use this method to get the number of members in a chat.
  *
  * @param  {Number|String} chatId  Unique identifier for the target group or username of the target supergroup
  * @param  {Object} [options] Additional Telegram query options
  * @return {Promise} Int on success
  * @see https://core.telegram.org/bots/api#getchatmembercount
  */
  getChatMemberCount(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('getChatMemberCount', { form });
  }

  /**
   * Use this method to get information about a member of a chat.
   *
   * @param  {Number|String} chatId  Unique identifier for the target group or username of the target supergroup
   * @param  {Number} userId  Unique identifier of the target user
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} [ChatMember](https://core.telegram.org/bots/api#chatmember) object on success
   * @see https://core.telegram.org/bots/api#getchatmember
   */
  getChatMember(chatId, userId, form = {}) {
    form.chat_id = chatId;
    form.user_id = userId;
    return this._request('getChatMember', { form });
  }

  /**
   * Use this method to set a new group sticker set for a supergroup.
   *
   * The bot **must be an administrator in the chat** for this to work and must have the appropriate administrator rights.
   *
   * **Note:** Use the field `can_set_sticker_set` optionally returned in [getChat](https://core.telegram.org/bots/api#getchat) requests to check if the bot can use this method.
   *
   * @param  {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
   * @param  {String} stickerSetName Name of the sticker set to be set as the group sticker set
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setchatstickerset
   */
  setChatStickerSet(chatId, stickerSetName, form = {}) {
    form.chat_id = chatId;
    form.sticker_set_name = stickerSetName;
    return this._request('setChatStickerSet', { form });
  }


  /**
   * Use this method to delete a group sticker set from a supergroup.
   *
   * Use the field `can_set_sticker_set` optionally returned in [getChat](https://core.telegram.org/bots/api#getchat) requests to check if the bot can use this method.
   *
   * @param  {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#deletechatstickerset
   */
  deleteChatStickerSet(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('deleteChatStickerSet', { form });
  }

  /**
   * Use this method to get custom emoji stickers, which can be used as a forum topic icon by any user.
   *
   * @param  {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} Array of [Sticker](https://core.telegram.org/bots/api#sticker) objects
   * @see https://core.telegram.org/bots/api#getforumtopiciconstickers
   */
  getForumTopicIconStickers(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('getForumTopicIconStickers', { form });
  }

  /**
   * Use this method to create a topic in a forum supergroup chat.
   * The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights.
   *
   * Returns information about the created topic as a [ForumTopic](https://core.telegram.org/bots/api#forumtopic) object.
   *
   * @param  {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
   * @param  {String} name Topic name, 1-128 characters
   * @param  {Object} [options] Additional Telegram query options
   * @see https://core.telegram.org/bots/api#createforumtopic
   */
  createForumTopic(chatId, name, form = {}) {
    form.chat_id = chatId;
    form.name = name;
    return this._request('createForumTopic', { form });
  }

  /**
   * Use this method to edit name and icon of a topic in a forum supergroup chat.
   * The bot must be an administrator in the chat for this to work and must have can_manage_topics administrator rights, unless it is the creator of the topic.
   *
   * @param {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
   * @param {Number} messageThreadId Unique identifier for the target message thread of the forum topic
   * @param {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#editforumtopic
   */
  editForumTopic(chatId, messageThreadId, form = {}) {
    form.chat_id = chatId;
    form.message_thread_id = messageThreadId;
    return this._request('editForumTopic', { form });
  }

  /**
   * Use this method to close an open topic in a forum supergroup chat.
   * The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights, unless it is the creator of the topic.
   *
   * @param {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
   * @param {Number} messageThreadId Unique identifier for the target message thread of the forum topic
   * @param {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#closeforumtopic
   */
  closeForumTopic(chatId, messageThreadId, form = {}) {
    form.chat_id = chatId;
    form.message_thread_id = messageThreadId;
    return this._request('closeForumTopic', { form });
  }

  /**
   * Use this method to reopen a closed topic in a forum supergroup chat.
   * The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights, unless it is the creator of the topic.
   *
   * @param {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
   * @param {Number} messageThreadId Unique identifier for the target message thread of the forum topic
   * @param {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#reopenforumtopic
   */
  reopenForumTopic(chatId, messageThreadId, form = {}) {
    form.chat_id = chatId;
    form.message_thread_id = messageThreadId;
    return this._request('reopenForumTopic', { form });
  }

  /**
   * Use this method to delete a forum topic along with all its messages in a forum supergroup chat.
   * The bot must be an administrator in the chat for this to work and must have the can_delete_messages administrator rights.
   *
   * @param {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
   * @param {Number} messageThreadId Unique identifier for the target message thread of the forum topic
   * @param {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#deleteforumtopic
   */
  deleteForumTopic(chatId, messageThreadId, form = {}) {
    form.chat_id = chatId;
    form.message_thread_id = messageThreadId;
    return this._request('deleteForumTopic', { form });
  }

  /**
   * Use this method to clear the list of pinned messages in a forum topic.
   * The bot must be an administrator in the chat for this to work and must have the can_pin_messages administrator right in the supergroup.
   *
   * @param {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
   * @param {Number} messageThreadId Unique identifier for the target message thread of the forum topic
   * @param {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#unpinallforumtopicmessages
   */
  unpinAllForumTopicMessages(chatId, messageThreadId, form = {}) {
    form.chat_id = chatId;
    form.message_thread_id = messageThreadId;
    return this._request('unpinAllForumTopicMessages', { form });
  }

  /**
  * Use this method to edit the name of the 'General' topic in a forum supergroup chat.
  * The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights.
  * The topic will be automatically unhidden if it was hidden.
  *
  * @param {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
  * @param {String} name New topic name, 1-128 characters
  * @param {Object} [options] Additional Telegram query options
  * @return {Promise} True on success
  * @see https://core.telegram.org/bots/api#editgeneralforumtopic
  */
  editGeneralForumTopic(chatId, name, form = {}) {
    form.chat_id = chatId;
    form.name = name;
    return this._request('editGeneralForumTopic', { form });
  }

  /**
  * Use this method to close an open 'General' topic in a forum supergroup chat.
  * The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights.
  * The topic will be automatically unhidden if it was hidden.
  *
  * @param {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
  * @param {Object} [options] Additional Telegram query options
  * @return {Promise} True on success
  * @see https://core.telegram.org/bots/api#closegeneralforumtopic
  */
  closeGeneralForumTopic(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('closeGeneralForumTopic', { form });
  }

  /**
  * Use this method to reopen a closed 'General' topic in a forum supergroup chat.
  * The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights.
  * The topic will be automatically unhidden if it was hidden.
  *
  * @param {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
  * @param {Object} [options] Additional Telegram query options
  * @return {Promise} True on success
  * @see https://core.telegram.org/bots/api#reopengeneralforumtopic
  */
  reopenGeneralForumTopic(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('reopenGeneralForumTopic', { form });
  }

  /**
  * Use this method to hide the 'General' topic in a forum supergroup chat.
  * The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights.
  * The topic will be automatically closed if it was open.
  *
  * @param {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
  * @param {Object} [options] Additional Telegram query options
  * @return {Promise} True on success
  * @see https://core.telegram.org/bots/api#hidegeneralforumtopic
  */
  hideGeneralForumTopic(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('hideGeneralForumTopic', { form });
  }

  /**
   * Use this method to unhide the 'General' topic in a forum supergroup chat.
   * The bot must be an administrator in the chat for this to work and must have the can_manage_topics administrator rights
   *
   * @param {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
   * @param {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#unhidegeneralforumtopic
   */
  unhideGeneralForumTopic(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('unhideGeneralForumTopic', { form });
  }

  /**
   * Use this method to clear the list of pinned messages in a General forum topic.
   * The bot must be an administrator in the chat for this to work and must have the can_pin_messages administrator right in the supergroup.
   *
   * @param {Number|String} chatId Unique identifier for the target group or username of the target supergroup (in the format @supergroupusername)
   * @param {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#unpinallgeneralforumtopicmessages
   */
  unpinAllGeneralForumTopicMessages(chatId, form = {}) {
    form.chat_id = chatId;
    return this._request('unhideGeneralForumTopic', { form });
  }

  /**
   * Use this method to send answers to callback queries sent from
   * [inline keyboards](https://core.telegram.org/bots#inline-keyboards-and-on-the-fly-updating).
   *
   * The answer will be displayed to the user as a notification at the top of the chat screen or as an alert.
   *
   * This method has **older, compatible signatures ([1][answerCallbackQuery-v0.27.1])([2][answerCallbackQuery-v0.29.0])**
   * that are being deprecated.
   *
   * @param  {String} callbackQueryId Unique identifier for the query to be answered
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#answercallbackquery
   */
  answerCallbackQuery(callbackQueryId, form = {}) {
    /* The older method signature (in/before v0.27.1) was answerCallbackQuery(callbackQueryId, text, showAlert).
     * We need to ensure backwards-compatibility while maintaining
     * consistency of the method signatures throughout the library */
    if (typeof form !== 'object') {
      /* eslint-disable no-param-reassign, prefer-rest-params */
      deprecate('The method signature answerCallbackQuery(callbackQueryId, text, showAlert) has been deprecated since v0.27.1');
      form = {
        callback_query_id: arguments[0],
        text: arguments[1],
        show_alert: arguments[2],
      };
      /* eslint-enable no-param-reassign, prefer-rest-params */
    }
    /* The older method signature (in/before v0.29.0) was answerCallbackQuery([options]).
     * We need to ensure backwards-compatibility while maintaining
     * consistency of the method signatures throughout the library. */
    if (typeof callbackQueryId === 'object') {
      /* eslint-disable no-param-reassign, prefer-rest-params */
      deprecate('The method signature answerCallbackQuery([options]) has been deprecated since v0.29.0');
      form = callbackQueryId;
      /* eslint-enable no-param-reassign, prefer-rest-params */
    } else {
      form.callback_query_id = callbackQueryId;
    }
    return this._request('answerCallbackQuery', { form });
  }

  /**
   * Use this method to get the list of boosts added to a chat by a use.
   * Requires administrator rights in the chat
   *
   * @param  {Number|String} chatId  Unique identifier for the group/channel
   * @param  {Number} userId Unique identifier of the target user
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, returns a [UserChatBoosts](https://core.telegram.org/bots/api#userchatboosts) object
   * @see https://core.telegram.org/bots/api#getuserchatboosts
   */
  getUserChatBoosts(chatId, userId, form = {}) {
    form.chat_id = chatId;
    form.user_id = userId;
    return this._request('getUserChatBoosts', { form });
  }

  /**
   * Use this method to get information about the connection of the bot with a business account
   *
   * @param  {Number|String} businessConnectionId  Unique identifier for the group/channel
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, returns [BusinessConnection](https://core.telegram.org/bots/api#businessconnection) object
   * @see https://core.telegram.org/bots/api#getbusinessconnection
   */
  getBusinessConnection(businessConnectionId, form = {}) {
    form.business_connection_id = businessConnectionId;
    return this._request('getBusinessConnection', { form });
  }

  /**
   * Use this method to change the list of the bot's commands.
   *
   * See https://core.telegram.org/bots#commands for more details about bot commands
   *
   * @param  {Array} commands  List of bot commands to be set as the list of the [bot's commands](https://core.telegram.org/bots/api#botcommand). At most 100 commands can be specified.
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setmycommands
   */
  setMyCommands(commands, form = {}) {
    form.commands = stringify(commands);

    if (form.scope) {
      form.scope = stringify(form.scope);
    }

    return this._request('setMyCommands', { form });
  }

  /**
   * Use this method to delete the list of the bot's commands for the given scope and user language.
   *
   *  After deletion, [higher level commands](https://core.telegram.org/bots/api#determining-list-of-commands) will be shown to affected users.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#deletemycommands
   */
  deleteMyCommands(form = {}) {
    return this._request('deleteMyCommands', { form });
  }


  /**
   * Use this method to get the current list of the bot's commands for the given scope and user language.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} Array of [BotCommand](https://core.telegram.org/bots/api#botcommand) on success. If commands aren't set, an empty list is returned.
   * @see https://core.telegram.org/bots/api#getmycommands
   */
  getMyCommands(form = {}) {
    if (form.scope) {
      form.scope = stringify(form.scope);
    }
    return this._request('getMyCommands', { form });
  }

  /**
   * Use this method to change the bot's name.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setmyname
   */
  setMyName(form = {}) {
    return this._request('setMyName', { form });
  }

  /**
   * Use this method to get the current bot name for the given user language.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} [BotName](https://core.telegram.org/bots/api#botname) on success
   * @see https://core.telegram.org/bots/api#getmyname
   */
  getMyName(form = {}) {
    return this._request('getMyName', { form });
  }

  /**
   * Use this method to change the bot's description, which is shown in the chat with the bot if the chat is empty.
   *
   * Returns True on success.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setmydescription
   */
  setMyDescription(form = {}) {
    return this._request('setMyDescription', { form });
  }

  /**
   * Use this method to get the current bot description for the given user language.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} Returns [BotDescription](https://core.telegram.org/bots/api#botdescription) on success.
   * @see https://core.telegram.org/bots/api#getmydescription
   */
  getMyDescription(form = {}) {
    return this._request('getMyDescription', { form });
  }

  /**
   * Use this method to change the bot's short description, which is shown on the bot's profile page
   * and is sent together with the link when users share the bot.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} Returns True on success.
   * @see https://core.telegram.org/bots/api#setmyshortdescription
   */
  setMyShortDescription(form = {}) {
    return this._request('setMyShortDescription', { form });
  }

  /**
   * Use this method to get the current bot short description for the given user language.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} Returns [BotShortDescription](https://core.telegram.org/bots/api#botshortdescription) on success.
   * @see https://core.telegram.org/bots/api#getmyshortdescription
   */
  getMyShortDescription(form = {}) {
    return this._request('getMyShortDescription', { form });
  }

  /**
   * Use this method to change the bot's menu button in a private chat, or the default menu button.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setchatmenubutton
   */
  setChatMenuButton(form = {}) {
    return this._request('setChatMenuButton', { form });
  }

  /**
   * Use this method to get the current value of the bot's menu button in a private chat, or the default menu button.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} [MenuButton](https://core.telegram.org/bots/api#menubutton) on success
   * @see https://core.telegram.org/bots/api#getchatmenubutton
   */
  getChatMenuButton(form = {}) {
    return this._request('getChatMenuButton', { form });
  }

  /**
   * Use this method to change the default administrator rights requested by the bot when it's added as an administrator to groups or channels.
   *
   * These rights will be suggested to users, but they are are free to modify the list before adding the bot.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#getchatmenubutton
   */
  setMyDefaultAdministratorRights(form = {}) {
    return this._request('setMyDefaultAdministratorRights', { form });
  }

  /**
   * Use this method to get the current default administrator rights of the bot.
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} [ChatAdministratorRights](https://core.telegram.org/bots/api#chatadministratorrights) on success
   * @see https://core.telegram.org/bots/api#getmydefaultadministratorrights
   */
  getMyDefaultAdministratorRights(form = {}) {
    return this._request('getMyDefaultAdministratorRights', { form });
  }

  /**
   * Use this method to edit text or [game](https://core.telegram.org/bots/api#games) messages sent by the bot or via the bot (for inline bots).
   *
   * Note: that **you must provide one of chat_id, message_id, or inline_message_id** in your request.
   *
   * @param  {String} text  New text of the message
   * @param  {Object} [options] Additional Telegram query options (provide either one of chat_id, message_id, or inline_message_id here)
   * @return {Promise} On success, if the edited message is not an inline message, the edited [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned
   * @see https://core.telegram.org/bots/api#editmessagetext
   */
  editMessageText(text, form = {}) {
    form.text = text;
    return this._request('editMessageText', { form });
  }

  /**
   * Use this method to edit captions of messages sent by the bot or via the bot (for inline bots).
   *
   * Note: You **must provide one of chat_id, message_id, or inline_message_id** in your request.
   *
   * @param  {String} caption  New caption of the message
   * @param  {Object} [options] Additional Telegram query options (provide either one of chat_id, message_id, or inline_message_id here)
   * @return {Promise} On success, if the edited message is not an inline message, the edited [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned
   * @see https://core.telegram.org/bots/api#editmessagecaption
   */
  editMessageCaption(caption, form = {}) {
    form.caption = caption;
    return this._request('editMessageCaption', { form });
  }

  /**
   * Use this method to edit animation, audio, document, photo, or video messages.
   *
   * If a message is a part of a message album, then it can be edited only to a photo or a video.
   *
   * Otherwise, message type can be changed arbitrarily. When inline message is edited, new file can't be uploaded.
   * Use previously uploaded file via its file_id or specify a URL.
   *
   * Note: You **must provide one of chat_id, message_id, or inline_message_id** in your request.
   *
   * @param  {Object} media  A JSON-serialized object for a new media content of the message
   * @param  {Object} [options] Additional Telegram query options (provide either one of chat_id, message_id, or inline_message_id here)
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise} On success, if the edited message is not an inline message, the edited [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned
   * @see https://core.telegram.org/bots/api#editmessagemedia
   */
  editMessageMedia(media, form = {}, fileOptions = {}) {
    const opts = {
      qs: form,
    };
    
    // This is to keep a backward compatibility, that was undocumented so no-one may be acutally using it
    if (typeof media.media === 'string' && regexAttach.test(media.media)) {
      const regexAttach = /attach:\/\/.+/;

      opts.formData = {};

      const payload = Object.assign({}, media);
      delete payload.media;

      try {
        const attachName = String(0);
        const [formData] = this._formatSendData(
          attachName,
          media.media.replace('attach://', ''),
          media.fileOptions
        );

        if (formData) {
          opts.formData[attachName] = formData[attachName];
          payload.media = `attach://${attachName}`;
        } else {
          throw new errors.FatalError(`Failed to process the replacement action for your ${media.type}`);
        }
      } catch (ex) {
        return Promise.reject(ex);
      }

      opts.qs.media = stringify(payload);

      return this._request('editMessageMedia', opts);
    }

    try {
      const sendData = this._formatSendData(media.type, media.media, fileOptions);
      opts.formData = sendData[0];
      media.media = sendData[1] || "attach://"+media.type;
      opts.qs.media = stringify(media);
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('editMessageMedia', opts);
  }

  /**
   * Use this method to edit only the reply markup of messages sent by the bot or via the bot (for inline bots).
   *
   * Note: You **must provide one of chat_id, message_id, or inline_message_id** in your request.
   *
   * @param  {Object} replyMarkup  A JSON-serialized object for an inline keyboard.
   * @param  {Object} [options] Additional Telegram query options (provide either one of chat_id, message_id, or inline_message_id here)
   * @return {Promise} On success, if the edited message is not an inline message, the edited [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned
   * @see https://core.telegram.org/bots/api#editmessagetext
   */
  editMessageReplyMarkup(replyMarkup, form = {}) {
    form.reply_markup = replyMarkup;
    return this._request('editMessageReplyMarkup', { form });
  }


  /**
   * Use this method to stop a poll which was sent by the bot.
   *
   * @param  {Number|String} chatId  Unique identifier for the group/channel
   * @param  {Number} pollId Identifier of the original message with the poll
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, the stopped [Poll](https://core.telegram.org/bots/api#poll) is returned
   * @see https://core.telegram.org/bots/api#stoppoll
   */
  stopPoll(chatId, pollId, form = {}) {
    form.chat_id = chatId;
    form.message_id = pollId;
    return this._request('stopPoll', { form });
  }

  /**
   * Use this method to send static .WEBP, [animated](https://telegram.org/blog/animated-stickers) .TGS,
   * or [video](https://telegram.org/blog/video-stickers-better-reactions) .WEBM stickers.
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String|stream.Stream|Buffer} sticker A file path, Stream or Buffer.
   * Can also be a `file_id` previously uploaded. Stickers are WebP format files.
   * @param  {Object} [options] Additional Telegram query options
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) is returned
   * @see https://core.telegram.org/bots/api#sendsticker
   */
  sendSticker(chatId, sticker, options = {}, fileOptions = {}) {
    const opts = {
      qs: options
    };
    opts.qs.chat_id = chatId;
    try {
      const sendData = this._formatSendData('sticker', sticker, fileOptions);
      opts.formData = sendData[0];
      opts.qs.sticker = sendData[1];
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('sendSticker', opts);
  }

  /**
   * Use this method to get a sticker set.
   *
   * @param  {String} name Name of the sticker set
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, a [StickerSet](https://core.telegram.org/bots/api#stickerset) object is returned
   * @see https://core.telegram.org/bots/api#getstickerset
   */
  getStickerSet(name, form = {}) {
    form.name = name;
    return this._request('getStickerSet', { form });
  }

  /**
   * Use this method to get information about custom emoji stickers by their identifiers.
   *
   * @param  {Array} custom_emoji_ids List of custom emoji identifiers. At most 200 custom emoji identifiers can be specified.
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} Array of [Sticker](https://core.telegram.org/bots/api#sticker) objects.
   * @see https://core.telegram.org/bots/api#getcustomemojistickers
   */
  getCustomEmojiStickers(customEmojiIds, form = {}) {
    form.custom_emoji_ids = stringify(customEmojiIds);
    return this._request('getCustomEmojiStickers', { form });
  }

  /**
   * Use this method to upload a file with a sticker for later use in *createNewStickerSet* and *addStickerToSet* methods (can be used multiple
   * times).
   *
   * @param  {Number} userId User identifier of sticker file owner
   * @param  {String|stream.Stream|Buffer} sticker A file path or a Stream with the sticker in .WEBP, .PNG, .TGS, or .WEBM format. Can also be a `file_id` previously uploaded.
   * @param {String} stickerFormat Allow values:  `static`, `animated` or `video`
   * @param  {Object} [options] Additional Telegram query options
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise} On success, a [File](https://core.telegram.org/bots/api#file) object is returned
   * @see https://core.telegram.org/bots/api#uploadstickerfile
   */
  uploadStickerFile(userId, sticker, stickerFormat = 'static', options = {}, fileOptions = {}) {
    const opts = {
      qs: options,
    };
    opts.qs.user_id = userId;
    opts.qs.sticker_format = stickerFormat;

    try {
      const sendData = this._formatSendData('sticker', sticker, fileOptions);
      opts.formData = sendData[0];
      opts.qs.sticker = sendData[1];
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('uploadStickerFile', opts);
  }

  /**
   * Use this method to create new sticker set owned by a user.
   *
   * The bot will be able to edit the created sticker set.
   *
   * You must use exactly one of the fields *png_sticker*, *tgs_sticker*, or *webm_sticker*
   *
   * @param  {Number} userId User identifier of created sticker set owner
   * @param  {String} name Short name of sticker set, to be used in `t.me/addstickers/` URLs (e.g.,   *"animals"*). Can contain only english letters, digits and underscores.
   *  Must begin with a letter, can't contain consecutive underscores and must end in `"_by_<bot_username>"`. `<bot_username>` is case insensitive. 1-64 characters.
   * @param  {String} title Sticker set title, 1-64 characters
   * @param  {String|stream.Stream|Buffer} pngSticker Png image with the sticker, must be up to 512 kilobytes in size,
   *  dimensions must not exceed 512px, and either width or height must be exactly 512px.
   * @param  {String} emojis One or more emoji corresponding to the sticker
   * @param  {Object} [options] Additional Telegram query options
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise}  True on success
   * @see https://core.telegram.org/bots/api#createnewstickerset
   */
  createNewStickerSet(userId, name, title, pngSticker, emojis, options = {}, fileOptions = {}) {
    const opts = {
      qs: options,
    };
    opts.qs.user_id = userId;
    opts.qs.name = name;
    opts.qs.title = title;
    opts.qs.emojis = emojis;
    opts.qs.mask_position = stringify(options.mask_position);
    try {
      const sendData = this._formatSendData('png_sticker', pngSticker, fileOptions);
      opts.formData = sendData[0];
      opts.qs.png_sticker = sendData[1];
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('createNewStickerSet', opts);
  }

  /**
   * Use this method to add a new sticker to a set created by the bot.
   *
   * You must use exactly one of the fields *png_sticker*, *tgs_sticker*, or *webm_sticker*
   *
   * Animated stickers can be added to animated sticker sets and only to them
   *
   * Note:
   * - Emoji sticker sets can have up to 200 sticker
   * - Static or Animated sticker sets can have up to 120 stickers
   *
   * @param  {Number} userId User identifier of sticker set owner
   * @param  {String} name Sticker set name
   * @param  {String|stream.Stream|Buffer} sticker Png image with the sticker (must be up to 512 kilobytes in size,
   * dimensions must not exceed 512px, and either width or height must be exactly 512px, [TGS animation](https://core.telegram.org/stickers#animated-sticker-requirements)
   * with the sticker or [WEBM video](https://core.telegram.org/stickers#video-sticker-requirements) with the sticker.
   * @param  {String} emojis One or more emoji corresponding to the sticker
   * @param  {String} stickerType Allow values: `png_sticker`, `tgs_sticker`, or `webm_sticker`.
   * @param  {Object} [options] Additional Telegram query options
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise}  True on success
   * @see https://core.telegram.org/bots/api#addstickertoset
   */
  addStickerToSet(userId, name, sticker, emojis, stickerType = 'png_sticker', options = {}, fileOptions = {}) {
    const opts = {
      qs: options,
    };
    opts.qs.user_id = userId;
    opts.qs.name = name;
    opts.qs.emojis = emojis;
    opts.qs.mask_position = stringify(options.mask_position);

    if (typeof stickerType !== 'string' || ['png_sticker', 'tgs_sticker', 'webm_sticker'].indexOf(stickerType) === -1) {
      return Promise.reject(new Error('stickerType must be a string and the allow types is: png_sticker, tgs_sticker, webm_sticker'));
    }

    try {
      const sendData = this._formatSendData(stickerType, sticker, fileOptions);
      opts.formData = sendData[0];
      opts.qs[stickerType] = sendData[1];
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('addStickerToSet', opts);
  }

  /**
   * Use this method to move a sticker in a set created by the bot to a specific position.
   *
   * @param  {String} sticker File identifier of the sticker
   * @param  {Number} position New sticker position in the set, zero-based
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setstickerpositioninset
   */
  setStickerPositionInSet(sticker, position, form = {}) {
    form.sticker = sticker;
    form.position = position;
    return this._request('setStickerPositionInSet', { form });
  }

  /**
   * Use this method to delete a sticker from a set created by the bot.
   *
   * @param  {String} sticker File identifier of the sticker
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#deletestickerfromset
   * @todo Add tests for this method!
   */
  deleteStickerFromSet(sticker, form = {}) {
    form.sticker = sticker;
    return this._request('deleteStickerFromSet', { form });
  }

  /**
   * Use this method to replace an existing sticker in a sticker set with a new one
   *
   * @param  {Number} user_id User identifier of the sticker set owner
   * @param  {String} name Sticker set name
   * @param  {String} sticker File identifier of the sticker
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#replacestickerinset
   * @todo Add tests for this method!
   */
  replaceStickerInSet(userId, name, oldSticker, form = {}) {
    form.user_id = userId;
    form.name = name;
    form.old_sticker = oldSticker;
    return this._request('deleteStickerFromSet', { form });
  }


  /**
   * Use this method to change the list of emoji assigned to a regular or custom emoji sticker.
   *
   * The sticker must belong to a sticker set created by the bot.
   *
   * @param  {String} sticker File identifier of the sticker
   * @param { Array } emojiList A JSON-serialized list of 1-20 emoji associated with the sticker
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setstickeremojilist
   */
  setStickerEmojiList(sticker, emojiList, form = {}) {
    form.sticker = sticker;
    form.emoji_list = stringify(emojiList);
    return this._request('setStickerEmojiList', { form });
  }

  /**
   * Use this method to change the list of emoji assigned to a `regular` or `custom emoji` sticker.
   *
   * The sticker must belong to a sticker set created by the bot.
   *
   * @param  {String} sticker File identifier of the sticker
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setstickerkeywords
   */
  setStickerKeywords(sticker, form = {}) {
    form.sticker = sticker;
    if (form.keywords) {
      form.keywords = stringify(form.keywords);
    }
    return this._request('setStickerKeywords', { form });
  }

  /**
   * Use this method to change the [mask position](https://core.telegram.org/bots/api#maskposition) of a mask sticker.
   *
   * The sticker must belong to a sticker set created by the bot.
   *
   * @param  {String} sticker File identifier of the sticker
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setstickermaskposition
   */
  setStickerMaskPosition(sticker, form = {}) {
    form.sticker = sticker;
    if (form.mask_position) {
      form.mask_position = stringify(form.mask_position);
    }
    return this._request('setStickerMaskPosition', { form });
  }

  /**
   * Use this method to set the title of a created sticker set.
   *
   * The sticker must belong to a sticker set created by the bot.
   *
   * @param  {String} name Sticker set name
   * @param  {String} title Sticker set title, 1-64 characters
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setstickersettitle
   */
  setStickerSetTitle(name, title, form = {}) {
    form.name = name;
    form.title = title;
    return this._request('setStickerSetTitle', { form });
  }

  /**
   * Use this method to add a thumb to a set created by the bot.
   *
   * Animated thumbnails can be set for animated sticker sets only. Video thumbnails can be set only for video sticker sets only
   *
   * @param  {Number} userId User identifier of sticker set owner
   * @param  {String} name Sticker set name
   * @param  {String|stream.Stream|Buffer} thumbnail A .WEBP or .PNG image with the thumbnail,
   * must be up to 128 kilobytes in size and have width and height exactly 100px,
   * a TGS animation with the thumbnail up to 32 kilobytes in size or a WEBM video with the thumbnail up to 32 kilobytes in size.
   *
   * Pass a file_id as a String to send a file that already exists on the Telegram servers, pass an HTTP URL as a String for Telegram
   * to get a file from the Internet, or upload a new one. Animated sticker set thumbnails can't be uploaded via HTTP URL.
   * @param  {Object} [options] Additional Telegram query options
   * @param  {Object} [fileOptions] Optional file related meta-data
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setstickersetthumbnail
   */
  setStickerSetThumbnail(userId, name, thumbnail, options = {}, fileOptions = {}) {
    const opts = {
      qs: options,
    };
    opts.qs.user_id = userId;
    opts.qs.name = name;
    opts.qs.mask_position = stringify(options.mask_position);
    try {
      const sendData = this._formatSendData('thumbnail', thumbnail, fileOptions);
      opts.formData = sendData[0];
      opts.qs.thumbnail = sendData[1];
    } catch (ex) {
      return Promise.reject(ex);
    }
    return this._request('setStickerSetThumbnail', opts);
  }


  /**
   * Use this method to set the thumbnail of a custom emoji sticker set.
   *
   * The sticker must belong to a sticker set created by the bot.
   *
   * @param  {String} name Sticker set name
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#setcustomemojistickersetthumbnail
   */
  setCustomEmojiStickerSetThumbnail(name, form = {}) {
    form.name = name;
    return this._request('setCustomEmojiStickerSetThumbnail', { form });
  }

  /**
   * Use this method to delete a sticker set that was created by the bot.
   *
   * The sticker must belong to a sticker set created by the bot.
   *
   * @param  {String} name Sticker set name
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} True on success
   * @see https://core.telegram.org/bots/api#deletestickerset
   */
  deleteStickerSet(name, form = {}) {
    form.name = name;
    return this._request('deleteStickerSet', { form });
  }

  /**
   * Send answers to an inline query.
   *
   * Note: No more than 50 results per query are allowed.
   *
   * @param  {String} inlineQueryId Unique identifier of the query
   * @param  {InlineQueryResult[]} results An array of results for the inline query
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, True is returned
   * @see https://core.telegram.org/bots/api#answerinlinequery
   */
  answerInlineQuery(inlineQueryId, results, form = {}) {
    form.inline_query_id = inlineQueryId;
    form.results = stringify(results);
    return this._request('answerInlineQuery', { form });
  }

  /**
   * Use this method to set the result of an interaction with a [Web App](https://core.telegram.org/bots/webapps)
   * and send a corresponding message on behalf of the user to the chat from which the query originated.
   *
   * @param  {String} webAppQueryId Unique identifier for the query to be answered
   * @param  {InlineQueryResult} result object that represents one result of an inline query
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, a [SentWebAppMessage](https://core.telegram.org/bots/api#sentwebappmessage) object is returned
   * @see https://core.telegram.org/bots/api#answerwebappquery
   */
  answerWebAppQuery(webAppQueryId, result, form = {}) {
    form.web_app_query_id = webAppQueryId;
    form.result = stringify(result);
    return this._request('answerWebAppQuery', { form });
  }

  /**
   * Use this method to send an invoice.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String} title Product name, 1-32 characters
   * @param  {String} description Product description, 1-255 characters
   * @param  {String} payload Bot defined invoice payload, 1-128 bytes. This will not be displayed to the user, use for your internal processes.
   * @param  {String} providerToken Payments provider token, obtained via `@BotFather`
   * @param  {String} currency Three-letter ISO 4217 currency code
   * @param  {Array} prices Breakdown of prices
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) is returned
   * @see https://core.telegram.org/bots/api#sendinvoice
   */
  sendInvoice(chatId, title, description, payload, providerToken, currency, prices, form = {}) {
    form.chat_id = chatId;
    form.title = title;
    form.description = description;
    form.payload = payload;
    form.provider_token = providerToken;
    form.currency = currency;
    form.prices = stringify(prices);
    form.provider_data = stringify(form.provider_data);
    if (form.suggested_tip_amounts) {
      form.suggested_tip_amounts = stringify(form.suggested_tip_amounts);
    }
    return this._request('sendInvoice', { form });
  }

  /**
  * Use this method to create a link for an invoice.
  *
  * @param {String} title Product name, 1-32 characters
  * @param {String} description Product description, 1-255 characters
  * @param {String} payload Bot defined invoice payload
  * @param {String} providerToken Payment provider token
  * @param {String} currency Three-letter ISO 4217 currency code
  * @param {Array} prices Breakdown of prices
  * @param {Object} [options] Additional Telegram query options
  * @returns {Promise} The created invoice link as String on success.
  * @see https://core.telegram.org/bots/api#createinvoicelink
  */
  createInvoiceLink(title, description, payload, providerToken, currency, prices, form = {}) {
    form.title = title;
    form.description = description;
    form.payload = payload;
    form.provider_token = providerToken;
    form.currency = currency;
    form.prices = stringify(prices);
    return this._request('createInvoiceLink', { form });
  }

  /**
  * Use this method to reply to shipping queries.
  *
  * If you sent an invoice requesting a shipping address and the parameter is_flexible was specified,
  * the Bot API will send an [Update](https://core.telegram.org/bots/api#update) with a shipping_query field to the bot
  *
  * @param  {String} shippingQueryId  Unique identifier for the query to be answered
  * @param  {Boolean} ok Specify if delivery of the product is possible
  * @param  {Object} [options] Additional Telegram query options
  * @return {Promise} On success, True is returned
  * @see https://core.telegram.org/bots/api#answershippingquery
  */
  answerShippingQuery(shippingQueryId, ok, form = {}) {
    form.shipping_query_id = shippingQueryId;
    form.ok = ok;
    form.shipping_options = stringify(form.shipping_options);
    return this._request('answerShippingQuery', { form });
  }

  /**
   * Use this method to respond to such pre-checkout queries
   *
   * Once the user has confirmed their payment and shipping details, the Bot API sends the final confirmation in the form of
   * an [Update](https://core.telegram.org/bots/api#update) with the field *pre_checkout_query*.
   *
   * **Note:** The Bot API must receive an answer within 10 seconds after the pre-checkout query was sent.
   *
   * @param  {String} preCheckoutQueryId  Unique identifier for the query to be answered
   * @param  {Boolean} ok Specify if every order details are ok
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, True is returned
   * @see https://core.telegram.org/bots/api#answerprecheckoutquery
   */
  answerPreCheckoutQuery(preCheckoutQueryId, ok, form = {}) {
    form.pre_checkout_query_id = preCheckoutQueryId;
    form.ok = ok;
    return this._request('answerPreCheckoutQuery', { form });
  }

  /**
   * Use this method for get the bot's Telegram Star transactions in chronological order
   *
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, returns a [StarTransactions](https://core.telegram.org/bots/api#startransactions) object
   * @see https://core.telegram.org/bots/api#getstartransactions
   */
  getStarTransactions(form = {}) {
    return this._request('getStarTransactions', { form });
  }

  /**
   * Use this method for refund a successful payment in [Telegram Stars](https://t.me/BotNews/90)
   *
   * @param  {Number} userId  Unique identifier of the user whose payment will be refunded
   * @param  {String} telegramPaymentChargeId Telegram payment identifier
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, True is returned
   * @see https://core.telegram.org/bots/api#refundstarpayment
   */
  refundStarPayment(userId, telegramPaymentChargeId, form = {}) {
    form.user_id = userId;
    form.telegram_payment_charge_id = telegramPaymentChargeId;
    return this._request('refundStarPayment', { form });
  }

  /**
   * Use this method to send a game.
   *
   * @param  {Number|String} chatId Unique identifier for the target chat or username of the target channel (in the format `@channelusername`)
   * @param  {String} gameShortName name of the game to be sent. Set up your games via `@BotFather`.
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, the sent [Message](https://core.telegram.org/bots/api#message) is returned
   * @see https://core.telegram.org/bots/api#sendgame
   */
  sendGame(chatId, gameShortName, form = {}) {
    form.chat_id = chatId;
    form.game_short_name = gameShortName;
    return this._request('sendGame', { form });
  }

  /**
   * Use this method to set the score of the specified user in a game message.
   *
   * @param  {Number} userId  Unique identifier of the target user
   * @param  {Number} score New score value, must be non-negative
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, if the message is not an inline message, the [Message](https://core.telegram.org/bots/api#message) is returned, otherwise True is returned
   * @see https://core.telegram.org/bots/api#setgamescore
   */
  setGameScore(userId, score, form = {}) {
    form.user_id = userId;
    form.score = score;
    return this._request('setGameScore', { form });
  }

  /**
   * Use this method to get data for high score tables.
   *
   * Will return the score of the specified user and several of their neighbors in a game.
   *
   * @param  {Number} userId  Unique identifier of the target user
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise} On success, returns an Array of [GameHighScore](https://core.telegram.org/bots/api#gamehighscore) objects
   * @see https://core.telegram.org/bots/api#getgamehighscores
   */
  getGameHighScores(userId, form = {}) {
    form.user_id = userId;
    return this._request('getGameHighScores', { form });
  }


  /**
 * Use this method to delete a message, including service messages, with the following limitations:
 * - A message can only be deleted if it was sent less than 48 hours ago.
 * - A dice message can only be deleted if it was sent more than 24 hours ago.
 * - Bots can delete outgoing messages in groups and supergroups.
 * - Bots can delete incoming messages in groups, supergroups and channels.
 * - Bots granted `can_post_messages` permissions can delete outgoing messages in channels.
 * - If the bot is an administrator of a group, it can delete any message there.
 * - If the bot has `can_delete_messages` permission in a supergroup, it can delete any message there.
 *
 * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format @channelusername)
 * @param  {Number} messageId  Unique identifier of the target message
 * @param  {Object} [options] Additional Telegram query options
 * @return {Promise} True on success
 * @see https://core.telegram.org/bots/api#deletemessage
 */
  deleteMessage(chatId, messageId, form = {}) {
    form.chat_id = chatId;
    form.message_id = messageId;
    return this._request('deleteMessage', { form });
  }

  /**
   * Use this method to delete multiple messages simultaneously. If some of the specified messages can't be found, they are skipped.
   *
   * @param  {Number|String} chatId  Unique identifier for the target chat or username of the target channel (in the format @channelusername)
   * @param  {Array<Number|String>} messageIds  Identifiers of 1-100 messages to delete. See deleteMessage for limitations on which messages can be deleted
   * @param  {Object} [options] Additional Telegram query options
   * @return {Promise<Boolean>} True on success
   * @see https://core.telegram.org/bots/api#deletemessages
   */
  deleteMessages(chatId, messageIds, form = {}) {
    form.chat_id = chatId;
    form.message_ids = stringify(messageIds);
    return this._request('deleteMessages', { form });
  }

}

module.exports = TelegramBot;
