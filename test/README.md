Running the tests:

```bash
# Token to be used
export TEST_TELEGRAM_TOKEN=<YOUR_BOT_TOKEN>

# User Id which you want to send the messages.
export TEST_USER_ID=<USER_ID>

# Group Id which to use in some of the tests, e.g. for TelegramBot#getChat()
export TEST_GROUP_ID=<GROUP_ID>

# Game short name to use in some tests, e.g. TelegramBot#sendGame()
# Defaults to "medusalab_test".
export TEST_GAME_SHORT_NAME=<GAME_SHORT_NAME>

# Sticker set name to use in some tests, e.g. TelegramBot#getStickerSet()
# Defaults to "pusheen".
export TEST_STICKER_SET_NAME=<STICKER_SET_NAME>

# Payment provider token to be used
export TEST_PROVIDER_TOKEN=<YOUR_PROVIDER_TOKEN>

# Run ALL tests
npm run test

# Run individual tests
npm run eslint              # static-analysis
```
Running the mocha:
```bash
npm run prepare-env
```
Edit the [.env](../.env) file by filling in the value of the environment variables with your data.

```bash
npm run mocha:dev           # mocha tests
```
Note: The bot must be an administrator in the chat for this to work and must have the appropriate admin rights.
