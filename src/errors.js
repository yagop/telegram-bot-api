exports.BaseError = class BaseError extends Error {
  /**
   * @class BaseError
   * @constructor
   * @private
   * @param  {String} code Error code
   * @param  {String} message Error message
   */
  constructor(code, message) {
    super(`${code}: ${message}`);
    this.code = code;
  }
  toJSON() {
    return {
      code: this.code,
      message: this.message,
    };
  }
};


exports.FatalError = class FatalError extends exports.BaseError {
  /**
   * Fatal Error. Error code is `"EFATAL"`.
   * @class FatalError
   * @constructor
   * @param  {String|Error} data Error object or message
   */
  constructor(data) {
    const error = (typeof data === 'string') ? null : data;
    const message = error ? error.message : data;
    super('EFATAL', message);
    if (error) {
      this.stack = error.stack;
      this.cause = error;
    }
  }
};


exports.ParseError = class ParseError extends exports.BaseError {
  /**
   * Error during parsing. Error code is `"EPARSE"`.
   * @class ParseError
   * @constructor
   * @param  {String} message Error message
   * @param  {http.IncomingMessage} response Server response
   */
  constructor(message, response) {
    super('EPARSE', message);
    this.response = response;
  }
};


exports.TelegramError = class TelegramError extends exports.BaseError {
  /**
   * Error returned from Telegram. Error code is `"ETELEGRAM"`.
   * @class TelegramError
   * @constructor
   * @param  {String} message Error message
   * @param  {http.IncomingMessage} response Server response
   */
  constructor(message, response) {
    super('ETELEGRAM', message);
    this.response = response;
  }
};
