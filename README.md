# insobot
Module based IRC bot written in C with markov chains and stuff.

See insobot.sh.example and src/config.h for configuration.

This table shows common modules & commands. Refer to the top of each module's
.c file for a complete listing.

|      Module      | Description             | Command                     | Purpose                           | Permission |
|------------------|-------------------------|-----------------------------|-----------------------------------|------------|
| **mod_admin**    | Administrative commands | fjoin      \<channel\>      | Force join a channel              | ADMIN      |
| **mod_alias**    | Adds short ! macros     | alias      \<key\> \<val\>  | Adds a channel-specific alias     | WLIST      |
|                  |                         | galias     \<key\> \<val\>  | Adds an alias for all channels    | WLIST      |
|                  |                         | unalias    \<key\>          | Removes a channel-specific alias  | WLIST      |
|                  |                         | gunalias   \<key\>          | Removes a global alias            | WLIST      |
|                  |                         | chaliasmod \<key\> \<perm\> | Sets permission to use the alias  | WLIST      |
|                  |                         | lsalias                     | Lists current aliases             | WLIST      |
| **mod_automod**  | Automatic moderation    | b  \<user\> [sec]           | Times out \<user\> (default 10s)  | WLIST      |
|                  |                         | ub \<user\>                 | Removes timeout on \<user\>       | WLIST      |
| **mod_haiku**    | Poorly generates haikus | haiku                       | Let the poetry flow               | WLIST      |
|                  |                         | scount \<word\>             | Show syllable guesstimate         | WLIST      |
| **mod_help**     | Lists commands          | help [module]               | Shows commands (for [module])     | NORMAL     |
| **mod_hmh**      | Handmade Hero commands  | schedule                    | Shows HMH schedule                | NORMAL     |
|                  |                         | time                        | Shows time until next HMH stream  | NORMAL     |
| **mod_karma**    | Tracks ++ and --        | karma                       | Shows your own karma              | NORMAL     |
|                  |                         | karma \<user\>              | Shows \<user\>'s karma            | WLIST      |
|                  |                         | ktop [n]                    | Shows overall top n karma         | ADMIN      |
| **mod_linkinfo** | Expands certain links   | *\<none\>*                  | *N/A*                             |            |
| **mod_markov**   | Says dumb things        | say                         | Force say something (5min cd)     | NORMAL     |
|                  |                         | ask                         | Asks something (shared 5min cd)   | NORMAL     |
|                  |                         | interval \<n\>              | Sets the rate of random messages  | ADMIN      |
|                  |                         | status                      | Prints markov status info         | ADMIN      |
| **mod_meta**     | Manages other modules   | m                           | Shows list of modules             | WLIST      |
|                  |                         | mon \<mod\>                 | Enables module \<mod\>            | WLIST      |
|                  |                         | moff \<mod\>                | Disables module \<mod\>           | WLIST      |
|                  |                         | minfo \<mod\>               | Show module's description         | WLIST      |
| **mod_quotes**   | Stores quotes           | q  [\#chan] \<n\>           | Shows quote n                     | NORMAL     |
|                  |                         | q+ [\#chan] \<text\>        | Adds a new quote                  | WLIST      |
|                  |                         | q- [\#chan] \<n\>           | Removes a quote                   | WLIST      |
|                  |                         | qfix [#chan] \<n\> \<text\> | Changes the text of quote n       | WLIST      |
|                  |                         | qft  [#chan] \<n\> \<time\> | Changes the timestamp of quote n  | WLIST      |
|                  |                         | ql                          | Shows link to quote gist          | NORMAL     |
|                  |                         | qs [\#chan] \<s\>           | Searches for quotes containing s  | NORMAL     |
|                  |                         | qr [\#chan]                 | Shows a random quote              | NORMAL     |
| **mod_twitch**   | Twitch.tv functions     | fnotify \<on\|off\>         | {En,Dis}ables follower notifier   | ADMIN      |
|                  |                         | uptime                      | Shows stream's uptime             | NORMAL     |
|                  |                         | vod                         | Links to the latest VoD           | NORMAL     |
|                  |                         | vod \<user\>                | Links to \<user\>'s latest VoD    | WLIST      |
| **mod_whitelist**| Manages permissions     | wl                          | Shows if you are whitelisted      | NORMAL     |
|                  |                         | iswl \<user\>               | Shows if \<user\> is whitelisted  | ADMIN      |
|                  |                         | wl+ \<user\>                | Whitelists \<user\>               | ADMIN      |
|                  |                         | wl- \<user\>                | Unwhitelists \<user\>             | ADMIN      |

