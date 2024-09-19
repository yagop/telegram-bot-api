const TelegramBot = require('..');
const request = require('@cypress/request-promise');
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const stream = require('stream');
const is = require('is');
const utils = require('./utils');
const isCI = require('is-ci');
const concat = require('concat-stream');

// Allows self-signed certificates to be used in our tests
process.env.NODE_TLS_REJECT_UNAUTHORIZED = '0';

const TOKEN = process.env.TEST_TELEGRAM_TOKEN;
if (!TOKEN) {
  throw new Error('Bot token not provided');
}

const PROVIDER_TOKEN = process.env.TEST_PROVIDER_TOKEN;
if (!PROVIDER_TOKEN && !isCI) { // If is not running in Travis / Appveyor
  throw new Error('Provider token not supplied');
}

// Telegram service if not User Id
const USERID = process.env.TEST_USER_ID || 777000;
const GROUPID = process.env.TEST_GROUP_ID || -1001075450562;
const GAME_SHORT_NAME = process.env.TEST_GAME_SHORT_NAME || 'medusalab_test';
const STICKER_SET_NAME = process.env.TEST_STICKER_SET_NAME || 'pusheen';
const CURRENT_TIMESTAMP = Date.now();
const timeout = 60 * 1000;
let portindex = 8091;
const staticPort = portindex++;
const pollingPort = portindex++;
const webHookPort = portindex++;
const pollingPort2 = portindex++;
const webHookPort2 = portindex++;
const badTgServerPort = portindex++;
const staticUrl = `http://127.0.0.1:${staticPort}`;
const key = `${__dirname}/../examples/ssl/key.pem`;
const cert = `${__dirname}/../examples/ssl/crt.pem`;
const ip = '216.58.210.174'; // Google IP ¯\_(ツ)_/¯
const lat = 47.5351072;
const long = -52.7508537;
const FILE_PATH = `${__dirname}/data/photo.png`;
let FILE_ID;
let GAME_CHAT_ID;
let GAME_MSG_ID;
let BOT_USERNAME;
let CHAT_INFO;
let STICKER_FILE_ID_FROM_SET;
let STICKERS_FROM_BOT_SET;

before(function beforeAll() {
  utils.startStaticServer(staticPort);
  return utils.startMockServer(pollingPort)
    .then(() => {
      return utils.startMockServer(pollingPort2);
    }).then(() => {
      return utils.startMockServer(badTgServerPort, { bad: true });
    });
});


describe('module.exports', function moduleExportsSuite() {
  const nodeVersion = parseInt(process.versions.node.split('.')[0], 10);
  it('is loaded from src/ on Node.js v6+ and above', function test() {
    if (nodeVersion <= 5) this.skip(); // skip on Node.js v5 and below
    assert.strictEqual(TelegramBot, require('../src/telegram'));
  });
  it('is loaded from lib/ on Node.js v5 and below', function test() {
    if (nodeVersion > 5) this.skip(); // skip on newer versions
    assert.strictEqual(TelegramBot, require('../lib/telegram'));
  });
});


describe('TelegramBot', function telegramSuite() {
  let bot;
  let testbot;
  let botPolling;
  let botWebHook;

  before(function beforeAll() {
    this.timeout(timeout);
    bot = new TelegramBot(TOKEN);
    testbot = new TelegramBot(TOKEN, {
      baseApiUrl: `http://127.0.0.1:${pollingPort}`,
      polling: {
        autoStart: false,
      },
      webHook: {
        autoOpen: false,
        port: webHookPort,
      },
    });
    botPolling = new TelegramBot(TOKEN, {
      baseApiUrl: `http://127.0.0.1:${pollingPort2}`,
      polling: true,
    });
    botWebHook = new TelegramBot(TOKEN, {
      webHook: {
        port: webHookPort2,
      },
    });

    utils.handleRatelimit(bot, 'sendPhoto', this);
    utils.handleRatelimit(bot, 'sendMessage', this);
    utils.handleRatelimit(bot, 'sendGame', this);
    utils.handleRatelimit(bot, 'getMe', this);
    utils.handleRatelimit(bot, 'getChat', this);

    return bot.sendPhoto(USERID, FILE_PATH).then(resp => {
      FILE_ID = resp.photo[0].file_id;
      return bot.sendMessage(USERID, 'chat');
    }).then(resp => {
      GAME_CHAT_ID = resp.chat.id;
      return bot.sendGame(USERID, GAME_SHORT_NAME);
    }).then(resp => {
      GAME_MSG_ID = resp.message_id;
    }).then(() => {
      return bot.getMe().then(resp => {
        BOT_USERNAME = resp.username;
      });
    }).then(() =>
      bot.getChat(GROUPID).then(resp => {
        CHAT_INFO = resp;
      }));
  });

  it('automatically starts polling', function test() {
    assert.strictEqual(botPolling.isPolling(), true);
    return utils.isPollingMockServer(pollingPort2);
  });

  it('automatically opens webhook', function test() {
    assert.strictEqual(botWebHook.hasOpenWebHook(), true);
    return utils.hasOpenWebHook(webHookPort2);
  });

  it('does not automatically poll if "autoStart" is false', function test() {
    assert.strictEqual(testbot.isPolling(), false);
    return utils.isPollingMockServer(pollingPort, true);
  });

  it('does not automatically open webhook if "autoOpen" is false', function test() {
    assert.strictEqual(testbot.hasOpenWebHook(), false);
    return utils.hasOpenWebHook(webHookPort, true);
  });

  it('correctly deletes the webhook if polling', function test() {
    const myBot = new TelegramBot(TOKEN, {
      polling: { autoStart: false, params: { timeout: 0 } },
    });
    utils.handleRatelimit(myBot, 'setWebHook', this);
    myBot.on('polling_error', (error) => {
      assert.ifError(error);
    });
    return myBot.setWebHook(ip, {}).then(() => {
      return myBot.startPolling();
    }).then(() => {
      return myBot.stopPolling();
    });
  });

  describe('Events', function eventsSuite() {
    it('(polling) emits "message" on receiving message', function test(done) {
      botPolling.once('message', () => {
        return done();
      });
    });
    it('(polling) emits "polling_error" if error occurs during polling', function test(done) {
      const myBot = new TelegramBot(12345, { polling: true });
      myBot.once('polling_error', (error) => {
        assert.ok(error);
        assert.strictEqual(error.code, 'ETELEGRAM');
        return myBot.stopPolling().then(() => { done(); }).catch(done);
      });
    });
    it('(webhook) emits "message" on receiving message', function test(done) {
      botWebHook.once('message', () => {
        return done();
      });
      utils.sendWebHookMessage(webHookPort2, TOKEN);
    });
    it('(webhook) emits "webhook_error" if could not parse webhook request body', function test(done) {
      botWebHook.once('webhook_error', (error) => {
        assert.ok(error);
        assert.strictEqual(error.code, 'EPARSE');
        return done();
      });
      utils.sendWebHookMessage(webHookPort2, TOKEN, { update: 'unparseable!', json: false });
    });
  });

  describe('WebHook', function webHookSuite() {
    it('returns 200 OK for health endpoint', function test(done) {
      utils.sendWebHookRequest(webHookPort2, '/healthz').then(resp => {
        assert.strictEqual(resp, 'OK');
        return done();
      });
    });
    it('returns 401 error if token is wrong', function test(done) {
      utils.sendWebHookMessage(webHookPort2, 'wrong-token').catch(resp => {
        assert.strictEqual(resp.statusCode, 401);
        return done();
      });
    });
    it('only accepts POST method', function test() {
      const methods = ['GET', 'PUT', 'DELETE', 'OPTIONS'];
      return Promise.all(methods, (method) => {
        return utils.sendWebHookMessage(webHookPort2, TOKEN, {
          method,
        }).then(() => {
          throw new Error(`expected error with webhook ${method} request`);
        }).catch(resp => {
          if (!resp.statusCode) throw resp;
          if (resp.statusCode !== 418) throw new Error(`unexpected error: ${resp.body}`);
        });
      }); // Promise.each
    });
  });

  describe('WebHook HTTPS', function webHookHTTPSSuite() {
    const port = portindex++;
    let httpsbot;
    afterEach(function afterEach() {
      return httpsbot.closeWebHook();
    });
    it('is enabled, through options.key and options.cert', function test() {
      httpsbot = new TelegramBot(TOKEN, { webHook: { port, key, cert } });
      return utils.sendWebHookMessage(port, TOKEN, { https: true });
    });
    it('is enabled, through options.pfx');
    it('is enabled, through options.https', function test() {
      httpsbot = new TelegramBot(TOKEN, {
        webHook: {
          port,
          https: {
            key: fs.readFileSync(key),
            cert: fs.readFileSync(cert),
          },
        },
      });
      return utils.sendWebHookMessage(port, TOKEN, { https: true });
    });
  });

  describe('errors', function errorsSuite() {
    const botParse = new TelegramBot('useless-token', {
      baseApiUrl: `http://localhost:${badTgServerPort}`,
    });
    it('FatalError is thrown if token is missing', function test() {
      const myBot = new TelegramBot(null);
      return myBot.sendMessage(USERID, 'text').catch(error => {
        // FIX: assert.ok(error instanceof TelegramBot.errors.FatalError);
        assert.strictEqual(error.code, 'EFATAL');
        assert.ok(error.message.indexOf('not provided') > -1);
      });
    });
    it('FatalError is thrown if file-type of Buffer could not be determined', function test() {
      let buffer;
      try {
        buffer = Buffer.from('12345');
      } catch (ex) {
        buffer = new Buffer('12345');
      }
      return bot.sendPhoto(USERID, buffer).catch(error => {
        // FIX: assert.ok(error instanceof TelegramBot.errors.FatalError);
        assert.strictEqual(error.code, 'EFATAL');
        assert.ok(error.message.indexOf('Unsupported') > -1);
      });
    });
    it('FatalError is thrown on network error', function test() {
      const myBot = new TelegramBot('useless-token', {
        baseApiUrl: 'http://localhost:23', // are we sure this port is not bound to?
      });
      return myBot.getMe().catch(error => {
        // FIX: assert.ok(error instanceof TelegramBot.errors.FatalError);
        assert.strictEqual(error.code, 'EFATAL');
      });
    });
    it('ParseError is thrown if response body could not be parsed', function test() {
      botParse.sendMessage(USERID, 'text').catch(error => {
        // FIX: assert.ok(error instanceof TelegramBot.errors.ParseError);
        assert.strictEqual(error.code, 'EPARSE');
        assert.ok(typeof error.response === 'object');
        assert.ok(typeof error.response.body === 'string');
      });
    });
    it('TelegramError is thrown if error is from Telegram', function test() {
      return bot.sendMessage('404', 'text').catch(error => {
        // FIX: assert.ok(error instanceof TelegramBot.errors.TelegramError);
        assert.strictEqual(error.code, 'ETELEGRAM');
        assert.ok(typeof error.response === 'object');
        assert.ok(typeof error.response.body === 'object');
      });
    });
  });

  describe('#startPolling', function initPollingSuite() {
    it('initiates polling', function test() {
      return testbot.startPolling().then(() => {
        return utils.isPollingMockServer(pollingPort);
      });
    });
    it('returns error if using webhook', function test() {
      return botWebHook.startPolling().catch((err) => {
        // TODO: check for error in a better way
        // FIX: assert.ok(err instanceof TelegramBot.errors.FatalError);
        assert.strictEqual(err.code, 'EFATAL');
        assert.ok(err.message.indexOf('mutually exclusive') !== -1);
      });
    });
  });

  describe('#isPolling', function isPollingSuite() {
    it('returns true if bot is polling', function test() {
      assert.strictEqual(testbot.isPolling(), true);
      return utils.isPollingMockServer(pollingPort);
    });
    it('returns false if bot is not polling', function test() {
      return testbot.stopPolling().then(() => {
        assert.strictEqual(testbot.isPolling(), false);
        utils.clearPollingCheck(pollingPort);
        return utils.isPollingMockServer(pollingPort, true);
      });
    });
    after(function after() {
      return testbot.initPolling();
    });
  });

  describe('#stopPolling', function stopPollingSuite() {
    it('stops polling by bot', function test() {
      return testbot.stopPolling().then(() => {
        utils.clearPollingCheck(pollingPort);
        return utils.isPollingMockServer(pollingPort, true);
      });
    });
  });

  describe('#openWebHook', function openWebHookSuite() {
    it('opens webhook', function test() {
      return testbot.openWebHook().then(() => {
        return utils.hasOpenWebHook(webHookPort);
      });
    });
    it('returns error if using polling', function test() {
      return botPolling.openWebHook().catch((err) => {
        // TODO: check for error in a better way
        // FIX: assert.ok(err instanceof TelegramBot.errors.FatalError);
        assert.strictEqual(err.code, 'EFATAL');
        assert.ok(err.message.indexOf('mutually exclusive') !== -1);
      });
    });
  });

  describe('#hasOpenWebHook', function hasOpenWebHookSuite() {
    it('returns true if webhook is opened', function test() {
      assert.strictEqual(testbot.hasOpenWebHook(), true);
      return utils.hasOpenWebHook(webHookPort);
    });
    it('returns false if webhook is closed', function test() {
      testbot.closeWebHook().then(() => {
        assert.strictEqual(testbot.hasOpenWebHook(), false);
        return utils.hasOpenWebHook(webHookPort, true);
      });
    });
    after(function after() {
      return testbot.openWebHook();
    });
  });

  describe('#closeWebHook', function closeWebHookSuite() {
    it('closes webhook', function test() {
      testbot.closeWebHook().then(() => {
        return utils.hasOpenWebHook(webHookPort, true);
      });
    });
  });


  describe('#setWebHook', function setWebHookSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'setWebHook', this);
    });
    it('should set a webHook', function test() {
      return bot
        .setWebHook(ip, {})
        .then(resp => {
          assert.strictEqual(resp, true);
        });
    });
    it('should set a webHook with certificate', function test() {
      return bot
        .setWebHook(ip, { certificate: cert })
        .then(resp => {
          assert.strictEqual(resp, true);
        });
    });
    it('(v0.25.0 and lower) should set a webHook with certificate', function test() {
      return bot
        .setWebHook(ip, cert)
        .then(resp => {
          assert.strictEqual(resp, true);
        });
    });
    it('should delete the webHook', function test() {
      return bot
        .setWebHook('', {})
        .then(resp => {
          assert.strictEqual(resp, true);
        });
    });
  });

  describe('#getWebHookInfo', function getWebHookInfoSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'getWebHookInfo', this);
    });
    it('should return WebhookInfo', function test() {
      return bot.getWebHookInfo().then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.boolean(resp.has_custom_certificate));
        assert.ok(is.number(resp.pending_update_count));
      });
    });
  });

  describe('#deleteWebHook', function deleteWebHookSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'deleteWebHook', this);
    });
    it('should delete webhook', function test() {
      return bot.deleteWebHook().then(resp => {
        assert.strictEqual(resp, true);
      });
    });
  });

  describe('#getUpdates', function getUpdatesSuite() {
    const opts = {
      timeout: 0,
      limit: 10,
    };
    before(function before() {
      utils.handleRatelimit(bot, 'setWebHook', this);
      utils.handleRatelimit(bot, 'getUpdates', this);
      return bot.deleteWebHook();
    });
    it('should return an Array', function test() {
      return bot.getUpdates(opts).then(resp => {
        assert.strictEqual(Array.isArray(resp), true);
      });
    });
    it('(v0.25.0 and lower) should return an Array', function test() {
      return bot.getUpdates(opts.timeout, opts.limit).then(resp => {
        assert.strictEqual(Array.isArray(resp), true);
      });
    });
  });

  describe('#getMe', function getMeSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'getMe', this);
    });
    it('should return an User object', function test() {
      return bot.getMe().then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.number(resp.id));
        assert.ok(is.string(resp.first_name));
      });
    });
  });

  describe('#getFileLink', function getFileLinkSuite() {
    this.timeout(timeout);
    before(function before() {
      utils.handleRatelimit(bot, 'getFileLink', this);
    });
    it('should get a file link', function test() {
      return bot.getFileLink(FILE_ID)
        .then(fileURI => {
          assert.ok(is.string(fileURI));
          assert.ok(utils.isTelegramFileURI(fileURI));
        });
    });
  });

  describe('#getFileStream', function getFileStreamSuite() {
    this.timeout(timeout);
    before(function before() {
      // utils.handleRatelimit(bot, 'getFileStream', this);
    });
    it('should get a file stream', function test(done) {
      const fileStream = bot.getFileStream(FILE_ID);
      assert.ok(fileStream instanceof stream.Readable);
      assert.strictEqual(fileStream.path, FILE_ID);
      fileStream.on('info', (info) => {
        assert.ok(info);
        assert.ok(utils.isTelegramFileURI(info.uri), `${info.uri} is not a file URI`);
        fileStream.pipe(concat(function readFile(buffer) {
          buffer.equals(fs.readFileSync(FILE_PATH)); // sync :(
          return done();
        }));
      });
    });
  });

  describe('#downloadFile', function downloadFileSuite() {
    const downloadPath = os.tmpdir();
    this.timeout(timeout);
    before(function before() {
      utils.handleRatelimit(bot, 'downloadFile', this);
    });
    it('should download a file', function test() {
      return bot.downloadFile(FILE_ID, downloadPath)
        .then(filePath => {
          assert.ok(is.string(filePath));
          assert.strictEqual(path.dirname(filePath), downloadPath);
          assert.ok(fs.existsSync(filePath));
          fs.unlinkSync(filePath); // Delete file after test
        });
    });
  });

  describe('#onText', function onTextSuite() {
    it('should call `onText` callback on match', function test(done) {
      const regexp = /\/onText (.+)/;
      botWebHook.onText(regexp, (msg, match) => {
        assert.strictEqual(match[1], 'ECHO ALOHA');
        assert.ok(botWebHook.removeTextListener(regexp));
        return done();
      });
      utils.sendWebHookMessage(webHookPort2, TOKEN, {
        message: { text: '/onText ECHO ALOHA' },
      });
    });
    it('should reset the global regex state with each message', function test(done) {
      const regexp = /\/onText (.+)/g;
      botWebHook.onText(regexp, () => {
        assert.strictEqual(regexp.lastIndex, 0);
        assert.ok(botWebHook.removeTextListener(regexp));
        return done();
      });
      utils.sendWebHookMessage(webHookPort2, TOKEN, {
        message: { text: '/onText ECHO ALOHA' },
      });
    });
  });

  describe('#removeTextListener', function removeTextListenerSuite() {
    const regexp = /\/onText/;
    const regexp2 = /\/onText/;
    const callback = function noop() { };
    after(function after() {
      bot.removeTextListener(regexp);
      bot.removeTextListener(regexp2);
    });
    it('removes the right text-listener', function test() {
      bot.onText(regexp, callback);
      bot.onText(regexp2, callback);
      const textListener = bot.removeTextListener(regexp);
      assert.strictEqual(regexp, textListener.regexp);
    });
    it('returns `null` if missing', function test() {
      assert.strictEqual(null, bot.removeTextListener(/404/));
    });
  });

  describe.skip('#onReplyToMessage', function onReplyToMessageSuite() { });

  describe('#removeReplyListener', function removeReplyListenerSuite() {
    const chatId = -1234;
    const messageId = 1;
    const callback = function noop() { };
    it('returns the right reply-listener', function test() {
      const id = bot.onReplyToMessage(chatId, messageId, callback);
      const replyListener = bot.removeReplyListener(id);
      assert.strictEqual(id, replyListener.id);
      assert.strictEqual(chatId, replyListener.chatId);
      assert.strictEqual(messageId, replyListener.messageId);
      assert.strictEqual(callback, replyListener.callback);
    });
    it('returns `null` if missing', function test() {
      // NOTE: '0' is never a valid reply listener ID :)
      assert.strictEqual(null, bot.removeReplyListener(0));
    });
  });

  /** Telegram Bot API Methods */

  describe.skip('#logOut', function logOutSuite() { });

  describe.skip('#close', function closeSuite() { });

  describe('#sendMessage', function sendMessageSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendMessage', this);
    });
    it('should send a message', function test() {
      return bot.sendMessage(USERID, 'test').then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.number(resp.message_id));
      });
    });
  });

  describe('#forwardMessage', function forwardMessageSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendMessage', this);
      utils.handleRatelimit(bot, 'forwardMessage', this);
    });
    it('should forward a message', function test() {
      return bot.sendMessage(USERID, 'test').then(resp => {
        const messageId = resp.message_id;
        return bot.forwardMessage(USERID, USERID, messageId)
          .then(forwarded => {
            assert.ok(is.object(forwarded));
            assert.ok(is.number(forwarded.message_id));
          });
      });
    });
  });

  describe('#copyMessage', function copyMessageSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendMessage', this);
      utils.handleRatelimit(bot, 'copyMessage', this);
    });
    it('should send copy of a message', function test() {
      return bot.sendMessage(USERID, 'test').then(resp => {
        const messageId = resp.message_id;
        return bot.copyMessage(USERID, USERID, messageId)
          .then(copy => {
            assert.ok(is.object(copy));
            assert.ok(is.number(copy.message_id));
          });
      });
    });
  });

  describe('#sendPhoto', function sendPhotoSuite() {
    let photoId;
    this.timeout(timeout);
    before(function before() {
      utils.handleRatelimit(bot, 'sendPhoto', this);
    });
    it('should send a photo from file', function test() {
      const photo = `${__dirname}/data/photo.png`;
      return bot.sendPhoto(USERID, photo).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.array(resp.photo));
        photoId = resp.photo[0].file_id;
      });
    });
    it('should send a photo from id', function test() {
      // Send the same photo as before
      const photo = photoId;
      return bot.sendPhoto(USERID, photo).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.array(resp.photo));
      });
    });
    it('should send a photo from fs.readStream', function test() {
      const photo = fs.createReadStream(`${__dirname}/data/photo.png`);
      return bot.sendPhoto(USERID, photo).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.array(resp.photo));
      });
    });
    it('should send a photo from request Stream', function test() {
      const photo = request(`${staticUrl}/photo.png`);
      return bot.sendPhoto(USERID, photo).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.array(resp.photo));
      });
    });
    it('should send a photo from a Buffer', function test() {
      const photo = fs.readFileSync(`${__dirname}/data/photo.png`);
      return bot.sendPhoto(USERID, photo).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.array(resp.photo));
      });
    });
  });

  describe('#sendAudio', function sendAudioSuite() {
    let audioId;
    this.timeout(timeout);
    before(function before() {
      utils.handleRatelimit(bot, 'sendAudio', this);
    });
    it('should send an MP3 audio', function test() {
      const audio = `${__dirname}/data/audio.mp3`;
      return bot.sendAudio(USERID, audio).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.audio));
        audioId = resp.audio.file_id;
      });
    });
    it('should send an audio from id', function test() {
      // Send the same audio as before
      const audio = audioId;
      return bot.sendAudio(USERID, audio).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.audio));
      });
    });
    it('should send an audio from fs.readStream', function test() {
      const audio = fs.createReadStream(`${__dirname}/data/audio.mp3`);
      return bot.sendAudio(USERID, audio).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.audio));
      });
    });
    it('should send an audio from request Stream', function test() {
      const audio = request(`${staticUrl}/audio.mp3`);
      return bot.sendAudio(USERID, audio).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.audio));
      });
    });
    it('should send an audio from a Buffer', function test() {
      const audio = fs.readFileSync(`${__dirname}/data/audio.mp3`);
      return bot.sendAudio(USERID, audio).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.audio));
      });
    });
    it('should send an audio file with thumbnail', function test() {
      const audio = `${__dirname}/data/audio.mp3`;
      const thumbImg = `attach://${__dirname}/data/sticker_thumb.png`;

      return bot.sendAudio(USERID, audio, { thumbnail: thumbImg }).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.audio));
        assert.ok(is.object(resp.audio.thumbnail));
      });
    });
  });

  describe('#sendDocument', function sendDocumentSuite() {
    let documentId;
    this.timeout(timeout);
    before(function before() {
      utils.handleRatelimit(bot, 'sendDocument', this);
    });
    it('should send a document from file', function test() {
      const document = `${__dirname}/data/photo.gif`;
      return bot.sendDocument(USERID, document).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.document));
        documentId = resp.document.file_id;
      });
    });
    it('should send a document from id', function test() {
      // Send the same document as before
      const document = documentId;
      return bot.sendDocument(USERID, document).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.document));
      });
    });
    it('should send a document from fs.readStream', function test() {
      const document = fs.createReadStream(`${__dirname}/data/photo.gif`);
      return bot.sendDocument(USERID, document).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.document));
      });
    });
    it('should send a document from request Stream', function test() {
      const document = request(`${staticUrl}/photo.gif`);
      return bot.sendDocument(USERID, document).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.document));
      });
    });
    it('should send a document from a Buffer', function test() {
      const document = fs.readFileSync(`${__dirname}/data/photo.gif`);
      return bot.sendDocument(USERID, document).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.document));
      });
    });
  });

  describe('#sendVideo', function sendVideoSuite() {
    let videoId;
    this.timeout(timeout);
    before(function before() {
      utils.handleRatelimit(bot, 'sendVideo', this);
    });
    it('should send a video from file', function test() {
      const video = `${__dirname}/data/video.mp4`;
      return bot.sendVideo(USERID, video).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.video));
        videoId = resp.video.file_id;
      });
    });
    it('should send a video from id', function test() {
      // Send the same video as before
      return bot.sendVideo(USERID, videoId).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.video));
      });
    });
    it('should send a video from fs.readStream', function test() {
      const video = fs.createReadStream(`${__dirname}/data/video.mp4`);
      return bot.sendVideo(USERID, video).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.video));
      });
    });
    it('should send a video from request Stream', function test() {
      const video = request(`${staticUrl}/video.mp4`);
      return bot.sendVideo(USERID, video).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.video));
      });
    });
    it('should send a video from a Buffer', function test() {
      const video = fs.readFileSync(`${__dirname}/data/video.mp4`);
      return bot.sendVideo(USERID, video).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.video));
      });
    });
  });

  describe('#sendAnimation', function sendAnimationSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendAnimation', this);
    });
    it('should send a gif as an animation', function test() {
      return bot.sendAnimation(USERID, `${__dirname}/data/photo.gif`).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.document));
      });
    });
  });

  describe('#sendVoice', function sendVoiceSuite() {
    let voiceId;
    this.timeout(timeout);
    before(function before() {
      utils.handleRatelimit(bot, 'sendVoice', this);
    });
    it('should send a voice from file', function test() {
      const voice = `${__dirname}/data/voice.ogg`;
      return bot.sendVoice(USERID, voice).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.voice));
        voiceId = resp.voice.file_id;
      });
    });
    it('should send a voice from id', function test() {
      // Send the same voice as before
      return bot.sendVoice(USERID, voiceId).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.voice));
      });
    });
    it('should send a voice from fs.readStream', function test() {
      const voice = fs.createReadStream(`${__dirname}/data/voice.ogg`);
      return bot.sendVoice(USERID, voice).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.voice));
      });
    });
    it('should send a voice from request Stream', function test() {
      const voice = request(`${staticUrl}/voice.ogg`);
      return bot.sendVoice(USERID, voice).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.voice));
      });
    });
    it('should send a voice from a Buffer', function test() {
      const voice = fs.readFileSync(`${__dirname}/data/voice.ogg`);
      return bot.sendVoice(USERID, voice).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.voice));
      });
    });
  });


  describe('#sendVideoNote', function sendVideoNoteSuite() {
    let videoNoteId;
    this.timeout(timeout);
    before(function before() {
      utils.handleRatelimit(bot, 'sendVideoNote', this);
    });
    it('should send a video from file', function test() {
      const video = `${__dirname}/data/video.mp4`;
      return bot.sendVideoNote(USERID, video, { length: 5 }).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.video));
        videoNoteId = resp.video.file_id;
      });
    });
    it('should send a video from id', function test() {
      // Send the same videonote as before
      assert.ok(videoNoteId);
      return bot.sendVideoNote(USERID, videoNoteId, { length: 5 }).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.video));
      });
    });
    it('should send a video from fs.readStream', function test() {
      const video = fs.createReadStream(`${__dirname}/data/video.mp4`);
      return bot.sendVideoNote(USERID, video, { length: 5 }).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.video));
      });
    });
    it('should send a video from a Buffer', function test() {
      const video = fs.readFileSync(`${__dirname}/data/video.mp4`);
      return bot.sendVideoNote(USERID, video, { length: 5 }).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.video));
      });
    });
  });

  describe('#sendMediaGroup', function sendMediaGroupSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendMediaGroup', this);
    });
    it('should send group of photos/videos as album', function test() {
      return bot.sendMediaGroup(USERID, [
        {
          type: 'photo',
          media: `${__dirname}/data/photo.png`,
        },
        {
          type: 'video',
          media: `${__dirname}/data/video.mp4`,
        },
        {
          type: 'photo',
          media: FILE_ID,
        },
      ], {
        disable_notification: true,
      }).then(resp => {
        assert.ok(is.array(resp));
        assert.strictEqual(resp.length, 3);
      });
    });
  });

  describe('#sendLocation', function sendLocationSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendLocation', this);
    });
    it('should send a location', function test() {
      return bot.sendLocation(USERID, lat, long).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.location));
        assert.ok(is.number(resp.location.latitude));
        assert.ok(is.number(resp.location.longitude));
      });
    });
  });

  describe('#editMessageLiveLocation', function editMessageLiveLocationSuite() {
    let message;
    before(function before() {
      utils.handleRatelimit(bot, 'editMessageLiveLocation', this);
      const opts = { live_period: 86400 };
      return bot.sendLocation(USERID, lat, long, opts).then(resp => { message = resp; });
    });
    it('edits live location', function test() {
      const opts = { chat_id: USERID, message_id: message.message_id };
      return bot.editMessageLiveLocation(lat + 1, long + 1, opts).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.location));
        assert.ok(is.number(resp.location.latitude));
        assert.ok(is.number(resp.location.longitude));
      });
    });
  });

  describe.skip('#stopMessageLiveLocation', function editMessageLiveLocationSuite() {
    let message;
    before(function before() {
      utils.handleRatelimit(bot, 'stopMessageLiveLocation', this);
      return bot.sendLocation(USERID, lat, long, { live_period: 86400 })
        .then((resp) => {
          message = resp;
        });
    });
    it('stops location updates', function test() {
      const opts = { chat_id: USERID, message_id: message.message_id };
      return bot.stopMessageLiveLocation(opts).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.location));
        assert.ok(is.number(resp.location.latitude));
        assert.ok(is.number(resp.location.longitude));
      });
    });
  });


  describe('#sendVenue', function sendVenueSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendVenue', this);
    });
    it('should send a venue', function test() {
      const title = 'The Village Shopping Centre';
      const address = '430 Topsail Rd,St. John\'s, NL A1E 4N1, Canada';
      return bot.sendVenue(USERID, lat, long, title, address).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.venue));
        assert.ok(is.object(resp.venue.location));
        assert.ok(is.number(resp.venue.location.latitude));
        assert.ok(is.number(resp.venue.location.longitude));
        assert.ok(is.string(resp.venue.title));
        assert.ok(is.string(resp.venue.address));
      });
    });
  });


  // NOTE: We are skipping TelegramBot#sendContact() as the
  // corresponding rate-limits enforced by the Telegram servers
  // are too strict! During our initial tests, we were required
  // to retry after ~72000 secs (1200 mins / 20 hrs).
  // We surely can NOT wait for that much time during testing
  // (or in most practical cases for that matter!)
  describe.skip('#sendContact', function sendContactSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendContact', this);
    });
    it('should send a contact', function test() {
      const phoneNumber = '+1(000)000-000';
      const firstName = 'John Doe';
      return bot.sendContact(USERID, phoneNumber, firstName).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.contact));
        assert.ok(is.string(resp.contact.phone_number));
        assert.ok(is.string(resp.contact.first_name));
      });
    });
  });

  describe('#sendPoll', function sendPollSuite() {
    it('should send a Poll', function test() {
      const question = '¿Are you okey?';
      const answers = ['Yes', 'No'];
      const opts = { is_anonymous: true };
      return bot.sendPoll(GROUPID, question, answers, opts).then(resp => {
        assert.ok(is.object(resp));
      });
    });
    it('should send a Quiz', function test() {
      const question = '¿Are you okey?';
      const answers = ['Yes', 'No'];
      const opts = {
        is_anonymous: true,
        type: 'quiz',
        correct_option_id: 0
      };
      return bot.sendPoll(GROUPID, question, answers, opts).then(resp => {
        assert.ok(is.object(resp));
      });
    });
  });

  describe('#sendDice', function sendDiceSuite() {
    it('should send a Dice', function test() {
      return bot.sendDice(GROUPID).then(resp => {
        assert.ok(is.object(resp));
      });
    });
    it('should send a Dart', function test() {
      const opts = { emoji: '🎯' };
      return bot.sendDice(GROUPID, opts).then(resp => {
        assert.ok(is.object(resp));
      });
    });
  });

  describe('#sendChatAction', function sendChatActionSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendChatAction', this);
    });
    it('should send a chat action', function test() {
      const action = 'typing';
      return bot.sendChatAction(USERID, action).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
  });

  describe('#getUserProfilePhotos', function getUserProfilePhotosSuite() {
    const opts = {
      offset: 0,
      limit: 1,
    };
    before(function before() {
      utils.handleRatelimit(bot, 'getUserProfilePhotos', this);
    });
    it('should get user profile photos', function test() {
      return bot.getUserProfilePhotos(USERID, opts).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.number(resp.total_count));
        assert.ok(is.array(resp.photos));
      });
    });
    it('(v0.25.0 and lower) should get user profile photos', function test() {
      return bot.getUserProfilePhotos(USERID, opts.offset, opts.limit).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.number(resp.total_count));
        assert.ok(is.array(resp.photos));
      });
    });
  });

  describe('#getFile', function getFileSuite() {
    this.timeout(timeout);
    before(function before() {
      utils.handleRatelimit(bot, 'getFile', this);
    });
    it('should get a file', function test() {
      return bot.getFile(FILE_ID)
        .then(resp => {
          assert.ok(is.object(resp));
          assert.ok(is.string(resp.file_path));
        });
    });
  });

  describe.skip('#banChatMember', function banChatMemberSuite() { });

  describe.skip('#unbanChatMember', function unbanChatMemberSuite() { });

  describe.skip('#restrictChatMember', function restrictChatMemberSuite() { });

  describe.skip('#promoteChatMember', function promoteChatMemberSuite() { });

  describe.skip('#setChatAdministratorCustomTitle ', function setChatAdministratorCustomTitleSuite() {
    it('should set chat permissions', function test() {
      return bot.setChatAdministratorCustomTitle(GROUPID, USERID, 'Custom Name').then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe.skip('#banChatSenderChat', function banChatSenderChatSuite() { });

  describe.skip('#unbanChatSenderChat', function banChatSenderChatSuite() { });

  describe('#setChatPermissions ', function setChatPermissionsSuite() {
    it('should set chat permissions', function test() {
      const ChatPermissions = {
        can_send_messages: true,
        can_send_media_messages: true,
        can_send_polls: false,
        can_send_other_messages: false,
        can_add_web_page_previews: true,
        can_change_info: false,
        can_invite_users: false,
        can_pin_messages: true
      };
      return bot.setChatPermissions(GROUPID, ChatPermissions).then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#exportChatInviteLink', function exportChatInviteLinkSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'exportChatInviteLink', this);
    });
    it('should export the group invite link', function test() {
      return bot.exportChatInviteLink(GROUPID).then(resp => {
        assert(resp.match(/^https:\/\/t\.me\/.+$/i), 'is a telegram invite link');
      });
    });
  });

  describe('#createChatInviteLink', function createChatInviteLinkSuite() {
    let inviteLink;
    before(function before() {
      utils.handleRatelimit(bot, 'createChatInviteLink', this);
      utils.handleRatelimit(bot, 'editChatInviteLink', this);
      utils.handleRatelimit(bot, 'revokeChatInviteLink', this);
    });
    it('should create a chat invite link', function test() {
      return bot.createChatInviteLink(GROUPID).then(resp => {
        assert(resp.invite_link.match(/^https:\/\/t\.me\/.+$/i), 'is a telegram invite link');
        inviteLink = resp.invite_link;
      });
    });

    it('should edit chat invite link', function test() {
      return bot.editChatInviteLink(GROUPID, inviteLink, { member_limit: 3 }).then(resp => {
        assert.strictEqual(resp.member_limit, 3);
      });
    });

    it('should revoke chat invite link', function test() {
      return bot.revokeChatInviteLink(GROUPID, inviteLink).then(resp => {
        assert.strictEqual(resp.is_revoked, true);
      });
    });
  });

  describe.skip('#approveChatJoinRequest', function approveChatJoinRequestSuite() { });

  describe.skip('#declineChatJoinRequest', function declineChatJoinRequestSuite() { });

  describe('#setChatPhoto', function setChatPhotoSuite() {
    this.timeout(timeout);
    before(function before() {
      utils.handleRatelimit(bot, 'setChatPhoto', this);
    });
    it('should set a chat photo from file', function test() {
      const photo = `${__dirname}/data/chat_photo.png`;
      return bot.setChatPhoto(GROUPID, photo).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
    it('should set a chat photo from fs.readStream', function test() {
      const photo = fs.createReadStream(`${__dirname}/data/chat_photo.png`);
      return bot.setChatPhoto(GROUPID, photo).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
    it('should set a chat photo from request Stream', function test() {
      const photo = request(`${staticUrl}/chat_photo.png`);
      return bot.setChatPhoto(GROUPID, photo).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
    it('should set a chat photo from a Buffer', function test() {
      const photo = fs.readFileSync(`${__dirname}/data/chat_photo.png`);
      return bot.setChatPhoto(GROUPID, photo).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
  });

  describe('#deleteChatPhoto', function deleteChatPhotoSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'deleteChatPhoto', this);
    });
    it('should delete the chat photo', function test() {
      return bot.deleteChatPhoto(GROUPID).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
  });

  describe('#setChatTitle', function setChatTitleSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'setChatTitle', this);
    });
    it('should set the chat title', function test() {
      const random = Math.floor(Math.random() * 1000);
      return bot.setChatTitle(GROUPID, `ntba test group (random: ${random})`).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
  });

  describe('#setChatDescription', function setChatDescriptionSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'setChatDescription', this);
    });
    it('should set the chat description', function test() {
      const random = Math.floor(Math.random() * 1000);
      const description = `node-telegram-bot-api test group (random: ${random})`;
      return bot.setChatDescription(GROUPID, description).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
  });

  describe('#pinChatMessage', function pinChatMessageSuite() {
    let messageId;
    before(function before() {
      utils.handleRatelimit(bot, 'pinChatMessage', this);
      return bot.sendMessage(GROUPID, 'To be pinned').then(resp => {
        messageId = resp.message_id;
      });
    });
    it('should pin chat message', function test() {
      return bot.pinChatMessage(GROUPID, messageId).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
  });

  describe('#unpinChatMessage', function unpinChatMessageSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'unpinChatMessage', this);
    });
    it('should unpin chat message', function test() {
      return bot.unpinChatMessage(GROUPID).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
  });

  describe('#unpinAllChatMessages', function unpinAllChatMessagesSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'unpinAllChatMessages', this);
    });
    it('should unpin all chats messages', function test() {
      return bot.unpinAllChatMessages(GROUPID).then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe.skip('#leaveChat', function leaveChatSuite() { });

  describe('#getChat', function getChatSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'getChat', this);
    });
    it('should return a Chat object', function test() {
      return bot.getChat(USERID).then(resp => {
        assert.ok(is.object(resp));
      });
    });
  });

  describe('#getChatAdministrators', function getChatAdministratorsSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'getChatAdministrators', this);
    });
    it('should return an Array', function test() {
      return bot.getChatAdministrators(GROUPID).then(resp => {
        assert.ok(Array.isArray(resp));
      });
    });
  });

  describe('#getChatMemberCount', function getChatMemberCountSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'getChatMemberCount', this);
    });
    it('should return an Integer', function test() {
      return bot.getChatMemberCount(GROUPID).then(resp => {
        assert.ok(Number.isInteger(resp));
      });
    });
  });

  describe('#getChatMember', function getChatMemberSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'getChatMember', this);
    });
    it('should return a ChatMember', function test() {
      return bot.getChatMember(GROUPID, USERID).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.user));
        assert.ok(is.string(resp.status));
      });
    });
  });

  describe('#setChatStickerSet', function setChatStickerSetSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'setChatStickerSet', this);
      // Check if the chat can set sticker sets
      if (!CHAT_INFO.can_set_sticker_set) {
        this.skip();
      }
    });
    it('should return a Boolean', function test() {
      return bot.setChatStickerSet(GROUPID, STICKER_SET_NAME).then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#deleteChatStickerSet', function deleteChatStickerSetSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'deleteChatStickerSet', this);
      // Check if the chat can delete sticker sets
      if (!CHAT_INFO.can_set_sticker_set) {
        this.skip();
      }
    });
    it('should return a Boolean', function test() {
      return bot.deleteChatStickerSet(GROUPID).then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe.skip('#answerCallbackQuery', function answerCallbackQuerySuite() { });

  describe('#setMyCommands', function setMyCommandsSuite() {
    it('should set bot commands', function test() {
      const opts = [
        { command: 'eat', description: 'Command for eat' },
        { command: 'run', description: 'Command for run' }
      ];
      return bot.setMyCommands(opts).then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#deleteMyCommands', function deleteMyCommandsSuite() {
    it('should delete bot commands', function test() {
      return bot.deleteMyCommands().then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#setMyDescription', function getMyCommandsSuite() {
    it('should set bot description for users with a specific lang code', function test() {
      return bot.setMyDescription({ description: 'Bot description' }).then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
    it('should set bot description for Spanish users', function test() {
      return bot.setMyDescription({ description: 'Spanish bot description', language_code: 'es' }).then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#setMyName', function setMyNameSuite() {
    it('should set bot name for Spanish users', function test() {
      return bot.setMyName({ name: 'Spanish Bot', language_code: 'es' }).then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#getMyName', function setMyNameSuite() {
    it('should get bot name for Spanish users', function test() {
      return bot.getMyName({ language_code: 'es' }).then(resp => {
        assert.ok(is.equal(resp.name, 'Spanish Bot'));
      });
    });
  });

  describe('#getMyDescription', function getMyDescriptionSuite() {
    it('should get bot description for a user without lang code', function test() {
      return bot.getMyDescription().then(resp => {
        assert.ok(is.equal(resp.description, 'Bot description'));
      });
    });
    it('should get bot description for Spanish users', function test() {
      return bot.getMyDescription({ language_code: 'es' }).then(resp => {
        assert.ok(is.equal(resp.description, 'Spanish bot description'));
      });
    });
  });

  describe('#setMyShortDescription', function setMyShortDescriptionSuite() {
    it('should set sort bot description for a user without lang code', function test() {
      return bot.setMyShortDescription({ short_description: 'Bot sort description' }).then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
    it('should set sort description for Spanish users', function test() {
      return bot.setMyShortDescription({ short_description: 'Spanish bot sort description', language_code: 'es' }).then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#getMyShortDescription', function getMyShortDescriptionSuite() {
    it('should get bot sort description for a user without lang code', function test() {
      return bot.getMyShortDescription().then(resp => {
        assert.ok(is.equal(resp.short_description, 'Bot sort description'));
      });
    });
    it('should get bot sort description for Spanish users', function test() {
      return bot.getMyShortDescription({ language_code: 'es' }).then(resp => {
        assert.ok(is.equal(resp.short_description, 'Spanish bot sort description'));
      });
    });
  });

  describe('#getMyCommands', function getMyCommandsSuite() {
    it('should get bot commands', function test() {
      return bot.getMyCommands().then(resp => {
        assert.ok(is.array(resp));
      });
    });
  });

  describe('#setChatMenuButton', function setChatMenuButtonSuite() {
    it('should set chat menu button', function test() {
      return bot.setChatMenuButton({
        chat_id: USERID,
        menu_button: JSON.stringify({ type: 'web_app', text: 'Hello', web_app: { url: 'https://webappcontent.telegram.org/cafe' } }),
      })
        .then(resp => {
          assert.ok(is.boolean(resp));
        });
    });
  });

  describe('#getChatMenuButton', function getChatMenuButtonSuite() {
    it('should get chat menu button', function test() {
      return bot.getChatMenuButton({ chat_id: USERID }).then(resp => {
        assert.ok(is.equal(resp, {
          type: 'web_app',
          text: 'Hello',
          web_app: { url: 'https://webappcontent.telegram.org/cafe' }
        }));
      });
    });
  });

  describe('#setMyDefaultAdministratorRights', function setMyDefaultAdministratorRightsSuite() {
    it('should set default administrator rights', function test() {
      return bot.setMyDefaultAdministratorRights({
        rights: JSON.stringify({
          can_manage_chat: true,
          can_change_info: true,
          can_delete_messages: false,
          can_invite_users: true,
          can_restrict_members: false,
          can_pin_messages: true,
          can_promote_members: false,
          can_manage_video_chats: false,
          is_anonymous: false
        }),
        for_channels: false
      }).then(resp => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#getMyDefaultAdministratorRights', function getMyDefaultAdministratorRightsSuite() {
    it('should get my default administrator rights', function test() {
      return bot.getMyDefaultAdministratorRights({
        for_channels: false
      }).then(resp => {
        assert.ok(is.equal(resp, {
          can_manage_chat: true,
          can_change_info: true,
          can_delete_messages: false,
          can_invite_users: true,
          can_restrict_members: false,
          can_pin_messages: true,
          can_manage_topics: false,
          can_promote_members: false,
          can_manage_video_chats: false,
          can_post_stories: false,
          can_edit_stories: false,
          can_delete_stories: false,
          is_anonymous: false
        }));
      });
    });
  });

  describe('#editMessageText', function editMessageTextSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendMessage', this);
      utils.handleRatelimit(bot, 'editMessageText', this);
    });
    it('should edit a message sent by the bot', function test() {
      return bot.sendMessage(USERID, 'test').then(resp => {
        assert.strictEqual(resp.text, 'test');
        const opts = {
          chat_id: USERID,
          message_id: resp.message_id
        };
        return bot.editMessageText('edit test', opts).then(msg => {
          assert.strictEqual(msg.text, 'edit test');
        });
      });
    });
  });

  describe('#editMessageCaption', function editMessageCaptionSuite() {
    this.timeout(timeout);
    before(function before() {
      utils.handleRatelimit(bot, 'sendPhoto', this);
      utils.handleRatelimit(bot, 'editMessageCaption', this);
    });
    it('should edit a caption sent by the bot', function test() {
      const photo = `${__dirname}/data/photo.png`;
      const options = { caption: 'test caption' };
      return bot.sendPhoto(USERID, photo, options).then(resp => {
        assert.strictEqual(resp.caption, 'test caption');
        const opts = {
          chat_id: USERID,
          message_id: resp.message_id
        };
        return bot.editMessageCaption('new test caption', opts).then(msg => {
          assert.strictEqual(msg.caption, 'new test caption');
        });
      });
    });
  });

  describe('#editMessageMedia', function editMessageMediaSuite() {
    let photoId;
    let messageID;
    before(function before() {
      utils.handleRatelimit(bot, 'editMessageMedia', this);
      const photo = `${__dirname}/data/photo.png`;
      return bot.sendPhoto(USERID, photo).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.array(resp.photo));
        photoId = resp.photo[0].file_id;
        messageID = resp.message_id;
      });
    });
    it('should edit a media message', function nextTest() {
      return bot.editMessageMedia({ type: 'photo', media: photoId, caption: 'edited' }, { chat_id: USERID, message_id: messageID }).then(editedResp => {
        assert.ok(is.object(editedResp));
        assert.ok(is.string(editedResp.caption));
      });
    });
  });


  describe('#editMessageReplyMarkup', function editMessageReplyMarkupSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendMessage', this);
      utils.handleRatelimit(bot, 'editMessageReplyMarkup', this);
    });
    it('should edit previously-set reply markup', function test() {
      return bot.sendMessage(USERID, 'test').then(resp => {
        const replyMarkup = JSON.stringify({
          inline_keyboard: [[{
            text: 'Test button',
            callback_data: 'test'
          }]]
        });
        const opts = {
          chat_id: USERID,
          message_id: resp.message_id
        };
        return bot.editMessageReplyMarkup(replyMarkup, opts).then(msg => {
          // Keyboard markup is not returned, do a simple object check
          assert.ok(is.object(msg));
        });
      });
    });
  });

  describe('#stopPoll', function stopPollSuite() {
    let msg;
    before(function before() {
      utils.handleRatelimit(bot, 'stopPoll', this);
      return bot.sendPoll(GROUPID, '¿Poll for stop before?', ['Yes', 'No']).then(resp => {
        msg = resp;
      });
    });
    it('should stop a Poll', function test() {
      return bot.stopPoll(GROUPID, msg.message_id).then(resp => {
        assert.ok(is.boolean(resp.is_closed) && resp.is_closed === true);
      });
    }
    );
  });

  describe('#deleteMessage', function deleteMessageSuite() {
    let messageId;
    before(function before() {
      utils.handleRatelimit(bot, 'deleteMessage', this);
      return bot.sendMessage(USERID, 'To be deleted').then(resp => {
        messageId = resp.message_id;
      });
    });
    it('should delete message', function test() {
      return bot.deleteMessage(USERID, messageId).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
  });

  describe('#sendSticker', function sendStickerSuite() {
    let stickerId;
    this.timeout(timeout);
    before(function before() {
      utils.handleRatelimit(bot, 'sendSticker', this);
    });
    it('should send a sticker from file', function test() {
      const sticker = `${__dirname}/data/sticker.webp`;
      return bot.sendSticker(USERID, sticker).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.sticker));
        stickerId = resp.sticker.file_id;
      });
    });
    it('should send a sticker from id', function test() {
      // Send the same photo as before
      return bot.sendSticker(USERID, stickerId).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.sticker));
      });
    });
    it('should send a sticker from fs.readStream', function test() {
      const sticker = fs.createReadStream(`${__dirname}/data/sticker.webp`);
      return bot.sendSticker(USERID, sticker).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.sticker));
      });
    });
    it('should send a sticker from request Stream', function test() {
      const sticker = request(`${staticUrl}/sticker.webp`);
      return bot.sendSticker(USERID, sticker).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.sticker));
      });
    });
    it('should send a sticker from a Buffer', function test() {
      const sticker = fs.readFileSync(`${__dirname}/data/sticker.webp`);
      return bot.sendSticker(USERID, sticker).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.sticker));
      });
    });
  });

  describe('#uploadStickerFile', function sendPhotoSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'uploadStickerFile', this);
    });
    it('should upload a sticker from file', function test() {
      const sticker = `${__dirname}/data/sticker.png`;

      bot.uploadStickerFile(USERID, sticker).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.string(resp.file_id));
      });
    });
    // Other tests (eg. Buffer, URL) are skipped, because they rely on the same features as sendPhoto.
  });

  describe('#createNewStickerSet', function createNewStickerSetSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'createNewStickerSet', this);
    });

    it('should create a new sticker set', function test(done) {
      const sticker = `${__dirname}/data/sticker.png`;
      const stickerPackName = `s${CURRENT_TIMESTAMP}_by_${BOT_USERNAME}`;

      bot.createNewStickerSet(USERID, stickerPackName, 'Sticker Pack Title', sticker, '😍').then((resp) => {
        assert.ok(is.boolean(resp));
      });
      setTimeout(() => done(), 2000);
    });
  });

  describe('#getStickerSet', function getStickerSetSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'getStickerSet', this);
    });
    it('should get the sticker set given the name of the set', function test() {
      return bot.getStickerSet(STICKER_SET_NAME).then(resp => {
        assert.ok(is.object(resp));
        assert.strictEqual(resp.name.toLowerCase(), STICKER_SET_NAME);
        assert.ok(is.string(resp.title));
        assert.ok(is.string(resp.sticker_type));
        assert.ok(is.array(resp.stickers));
      });
    });
    // This test depends on the previous test createNewStickerSet
    it('should get the recent sticker set created given the name of the set', function test() {
      const stickerPackName = `s${CURRENT_TIMESTAMP}_by_${BOT_USERNAME}`;
      return bot.getStickerSet(stickerPackName).then(resp => {
        STICKER_FILE_ID_FROM_SET = resp.stickers[0].file_id;
        assert.ok(is.object(resp));
        assert.strictEqual(resp.name.toLowerCase(), stickerPackName.toLowerCase());
        assert.ok(is.string(resp.title));
        assert.ok(is.string(resp.sticker_type));
        assert.ok(is.array(resp.stickers));
      });
    });
  });

  describe('#getCustomEmojiStickers', function getCustomEmojiStickersSuite() {
    const CHERRY_EMOJI_STICKERS_ID = ['5380109565226391871', '5431711346724968789'];
    const STICKER_EMOJI_SET_NAME = 'CherryEmoji';

    it('should get the custom emoji stickers', function test() {
      return bot.getCustomEmojiStickers([CHERRY_EMOJI_STICKERS_ID[0]]).then(resp => {
        assert.ok(is.array(resp));
        assert.ok(is.object(resp[0]));
        assert.ok(is.string(resp[0].set_name) && resp[0].set_name === STICKER_EMOJI_SET_NAME);
        assert.ok(resp[0].custom_emoji_id === CHERRY_EMOJI_STICKERS_ID[0]);
      });
    });
    it('should get 2 custom emoji stickers', function test() {
      return bot.getCustomEmojiStickers(CHERRY_EMOJI_STICKERS_ID).then(resp => {
        assert.ok(is.array(resp) && resp.length === 2);
        assert.ok(is.object(resp[1]));
        assert.ok(is.string(resp[1].set_name) && resp[1].set_name === STICKER_EMOJI_SET_NAME);
        assert.ok(resp[1].custom_emoji_id === CHERRY_EMOJI_STICKERS_ID[1]);
      });
    });
  });


  describe('#addStickerToSet', function addStickerToSetSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'addStickerToSet', this);
    });

    it('should add a sticker to a set', function test() {
      const sticker = `${__dirname}/data/sticker.png`;
      const stickerPackName = `s${CURRENT_TIMESTAMP}_by_${BOT_USERNAME}`;

      bot.addStickerToSet(USERID, stickerPackName, sticker, '😊😍🤔', 'png_sticker').then((resp) => {
        assert.ok(is.boolean(resp));
      });
    });
    it('should add a sticker to a set using the file Id', function test(done) {
      const stickerPackName = `s${CURRENT_TIMESTAMP}_by_${BOT_USERNAME}`;

      bot.addStickerToSet(USERID, stickerPackName, STICKER_FILE_ID_FROM_SET, '😊🤔', 'png_sticker').then((resp) => {
        assert.ok(is.boolean(resp));
      });
      setTimeout(() => done(), 2000);
    });
  });

  describe('#setStickerPositionInSet', function setStickerPositionInSet() {
    before(function before() {
      utils.handleRatelimit(bot, 'setStickerPositionInSet', this);
    });
    it('should set the position of a sticker in a set', function test() {
      bot.setStickerPositionInSet(STICKER_FILE_ID_FROM_SET, 0).then((resp) => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#deleteStickerFromSet', function deleteStickerFromSetSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'deleteStickerFromSet', this);
    });
    it('should delete a sticker from a set', function test() {
      bot.deleteStickerFromSet(STICKER_FILE_ID_FROM_SET).then((resp) => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#setStickerEmojiList', function setStickerEmojiListSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'setStickerEmojiList', this);
    });

    it('should get the list for the given sticker of the bot sticker pack', function test(done) {
      const stickerPackName = `s${CURRENT_TIMESTAMP}_by_${BOT_USERNAME}`;

      bot.getStickerSet(stickerPackName).then(resp => {
        STICKERS_FROM_BOT_SET = resp.stickers;
        assert.ok(is.array(STICKERS_FROM_BOT_SET));
      });

      setTimeout(() => done(), 2000);
    });

    it('should set a emoji list for the given sticker', function test() {
      assert.ok(is.equal(STICKERS_FROM_BOT_SET[0].type, 'regular'));

      bot.setStickerEmojiList(STICKERS_FROM_BOT_SET[0].file_id, ['🥳', '😀', '😇']).then((resp) => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#setStickerKeywords', function setStickerKeywordsSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'setStickerKeywords', this);
    });
    it('should set a keywords list for the given sticker', function test() {
      assert.ok(is.equal(STICKERS_FROM_BOT_SET[0].type, 'regular'));
      bot.setStickerKeywords(STICKERS_FROM_BOT_SET[0].file_id, { keywords: ['house', 'cat'] }).then((resp) => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe.skip('#setStickerMaskPosition', function setStickerKeywordsSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'setStickerMaskPosition', this);
    });
    it('should delete a sticker from a set', function test() {
      bot.setStickerMaskPosition(STICKER_FILE_ID_FROM_SET, { point: 'eyes', scale: 2, x_shift: 1, y_shift: 1 }).then((resp) => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#setStickerSetTitle', function setStickerSetTitleSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'setStickerSetTitle', this);
    });
    it('should set a new sticker set title', function test() {
      const stickerPackName = `s${CURRENT_TIMESTAMP}_by_${BOT_USERNAME}`;

      bot.setStickerSetTitle(stickerPackName, 'New title').then((resp) => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe('#setStickerSetThumbnail', function setStickerSetThumbnailSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'setStickerSetThumbnail', this);
    });

    it('should set a sticker set thumbnail', function test() {
      const stickerThumb = `${__dirname}/data/sticker_thumb.png`;
      const stickerPackName = `s${CURRENT_TIMESTAMP}_by_${BOT_USERNAME}`;

      bot.setStickerSetThumbnail(USERID, stickerPackName, stickerThumb).then((resp) => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe.skip('#setCustomEmojiStickerSetThumbnail', function setCustomEmojiStickerSetThumbnailSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'setCustomEmojiStickerSetThumbnail', this);
    });

    it('should set a custom emoji sticjer set as thumbnail', function test() {
      const stickerPackName = `s${CURRENT_TIMESTAMP}_by_${BOT_USERNAME}`;

      bot.setCustomEmojiStickerSetThumbnail(stickerPackName, { custom_emoji_id: null }).then((resp) => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe.skip('#deleteStickerSet', function deleteStickerSetSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'deleteStickerSet', this);
    });

    it('should delete sticker set', function test() {
      const stickerPackName = `s${CURRENT_TIMESTAMP}_by_${BOT_USERNAME}`;

      bot.deleteStickerSet(stickerPackName).then((resp) => {
        assert.ok(is.boolean(resp));
      });
    });
  });

  describe.skip('#answerInlineQuery', function answerInlineQuerySuite() { });

  describe.skip('#answerWebAppQuery', function answerCallbackQuerySuite() { });

  describe('#sendInvoice', function sendInvoiceSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendInvoice', this);
    });
    it('should send an invoice', function test() {
      if (isCI) {
        this.skip(); // Skip test for now
      }
      const title = 'Demo product';
      const description = 'our test product';
      const payload = 'sku-p001';
      const providerToken = PROVIDER_TOKEN;
      const currency = 'USD';
      const prices = [{ label: 'product', amount: 11000 }, { label: 'tax', amount: 11000 }];
      return bot.sendInvoice(USERID, title, description, payload, providerToken, currency, prices).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.invoice));
        assert.ok(is.number(resp.invoice.total_amount));
      });
    });
  });

  describe('#createInvoiceLink', function createInvoiceLinkSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'createInvoiceLink', this);
    });
    it('should create an invoice link', function test() {
      if (isCI) {
        this.skip(); // Skip test for now
      }
      const title = 'Invoice link product';
      const description = 'Our test invoice link product';
      const payload = 'sku-p002';
      const providerToken = PROVIDER_TOKEN;
      const currency = 'EUR';
      const prices = [{ label: 'NTBA API', amount: 12000 }, { label: 'tax', amount: 10000 }];
      return bot.createInvoiceLink(title, description, payload, providerToken, currency, prices).then(resp => {
        assert.ok(is.string(resp));
      });
    });
  });

  describe.skip('#answerShippingQuery', function answerShippingQuerySuite() { });


  describe.skip('#answerPreCheckoutQuery', function answerPreCheckoutQuerySuite() { });

  describe('#sendGame', function sendGameSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'sendGame', this);
    });
    it('should send a Game', function test() {
      return bot.sendGame(USERID, GAME_SHORT_NAME).then(resp => {
        assert.ok(is.object(resp));
        assert.ok(is.object(resp.game));
      });
    });
  });

  describe('#setGameScore', function setGameScoreSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'setGameScore', this);
    });
    it('should set GameScore', function test() {
      const score = Math.floor(Math.random() * 1000);
      const opts = {
        chat_id: GAME_CHAT_ID,
        message_id: GAME_MSG_ID,
        force: true
      };
      return bot.setGameScore(USERID, score, opts).then(resp => {
        assert.ok(is.object(resp) || is.boolean(resp));
      });
    });
  });

  describe('#getGameHighScores', function getGameHighScoresSuite() {
    before(function before() {
      utils.handleRatelimit(bot, 'getGameHighScores', this);
    });
    it('should get GameHighScores', function test() {
      const opts = {
        chat_id: GAME_CHAT_ID,
        message_id: GAME_MSG_ID,
      };
      return bot.getGameHighScores(USERID, opts).then(resp => {
        assert.ok(is.array(resp));
      });
    });
  });

  describe('#setMessageReaction', function setMessageReactionSuite() {
    let messageId;
    const Reactions = [{ type: 'emoji', emoji: '👍' }];
    before(function before() {
      utils.handleRatelimit(bot, 'setMessageReaction', this);
      return bot.sendMessage(USERID, 'To be reacted').then(resp => {
        messageId = resp.message_id;
      });
    });
    it('should add reactions to message', function test() {
      return bot.setMessageReaction(USERID, messageId, { reaction: Reactions, is_big: true }).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
  });

  describe('#deleteMessages', function setMessageReactionSuite() {
    let messageId;
    before(function before() {
      utils.handleRatelimit(bot, 'deleteMessages', this);
      return bot.sendMessage(USERID, 'To be deleted').then(resp => {
        messageId = resp.message_id;
      });
    });
    it('should delete message from array', function test() {
      return bot.deleteMessages(USERID, [messageId]).then(resp => {
        assert.strictEqual(resp, true);
      });
    });
  });

  describe('#copyMessages', function setMessageReactionSuite() {
    let messageId;
    before(function before() {
      utils.handleRatelimit(bot, 'copyMessages', this);
      return bot.sendMessage(GROUPID, 'To be copyed').then(resp => {
        messageId = resp.message_id;
      });
    });
    it('should copy messages from array', function test() {
      return bot.copyMessages(USERID, GROUPID, [messageId]).then(resp => {
        assert.ok(is.array(resp));
        assert.ok(resp && resp.length === 1);
      });
    });
  });
}); // End Telegram
