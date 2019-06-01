#ifndef INSOBOT_CONFIG_H_
#define INSOBOT_CONFIG_H_

// main control char / prefix for commands
#define CONTROL_CHAR "!"

// alternatate control char for certain commands (can be left undefined)
//#define CONTROL_CHAR_2 "\\"

// note, this is overritten by the IRC_USER environment variable
#define DEFAULT_BOT_NAME "fake-insobot"

// minimum milliseconds between messages
#define CMD_RATE_LIMIT_MS 1500

// number of backed-up commands to keep
#define CMD_QUEUE_MAX 32

// URL to the schedule webpage if you're using mod_schedule / mod_twitter
#define SCHEDULE_URL ""

#endif
