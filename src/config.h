
// used in mod_markov + mod_admin to determine the main admin,
// and in mod_chans as the default channel to join (prefixed by #)
#define BOT_OWNER "insofaras"

// note, this is overritten by the IRC_USER environment variable
#define DEFAULT_BOT_NAME "fake-insobot"

// minimum milliseconds between messages
#define CMD_RATE_LIMIT_MS 1500

// number of backed-up commands to keep
#define CMD_QUEUE_MAX 32
