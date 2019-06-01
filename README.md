# insobot
Module based IRC bot written in C with markov chains and stuff.

## Running it
To run your own insobot instance, see [this quickstart guide](https://github.com/baines/insobot/wiki/quickstart)
, other pages in [the wiki](https://github.com/baines/insobot/wiki), and the
files 'insobot.sh.example' and 'src/config.h'.

I should be available in [#botdev](ircs://irc.handmade.network:7777/#botdev)
on irc.handmade.network/7777 (ssl) if you need help. See also the [Handmade Network project page](https://insobot.handmade.network)
for a place to post forum topics or read project updates.

## Contributing
Any contributions of new modules, improvements to existing modules, or raising
issues here on github is welcome.

If you want to write your own modules take a look at `src/module.h` for the API
and `src/mod_hello_world.c` for an example.

## Commands
This table shows common modules & commands. Refer to the top of each module's
.c file for a complete listing.

**NEW**: visit the (tounge-in-cheek) documentation website at https://insobot.church/
This site is auto-generated from each module's source code using the `ibdox` program in utils/

NOTE: The insobot instance on twitch prefixes most of these commands with '\\'
(backslash), the default prefix (set in config.h) is '!' (exclamation mark) however.

|      Module      | Description             | Command                     | Purpose                           | Permission |
|------------------|-------------------------|-----------------------------|-----------------------------------|------------|
| **mod_alias**    | Adds short ! macros     | alias      \<key\> \<val\>  | Adds a channel-specific alias     | WLIST      |
|                  |                         | galias     \<key\> \<val\>  | Adds an alias for all channels    | WLIST      |
|                  |                         | unalias    \<key\>          | Removes a channel-specific alias  | WLIST      |
|                  |                         | gunalias   \<key\>          | Removes a global alias            | WLIST      |
|                  |                         | chaliasmod \<key\> \<perm\> | Sets permission to use the alias  | WLIST      |
|                  |                         | lsalias                     | Lists current aliases             | WLIST      |
| **mod_automod**  | Automatic moderation    | b  \<user\> [sec]           | Times out \<user\> (default 10s)  | WLIST      |
|                  |                         | ub \<user\>                 | Removes timeout on \<user\>       | WLIST      |
| **mod_core**     | Core functionality      | m                           | Shows list of modules             | WLIST      |
|                  |                         | mon \<mod\>                 | Enables module \<mod\>            | WLIST      |
|                  |                         | moff \<mod\>                | Disables module \<mod\>           | WLIST      |
|                  |                         | minfo \<mod\>               | Show module's description         | WLIST      |
|                  |                         | join \<chan\>               | Joins the given channel           | ADMIN      |
|                  |                         | leave \<chan\>              | Leaves the current channel        | ADMIN      |
| **mod_haiku**    | Poorly generates haikus | haiku                       | Let the poetry flow               | WLIST      |
|                  |                         | scount \<word\>             | Show syllable guesstimate         | WLIST      |
| **mod_help**     | Lists commands          | help [module]               | Shows commands (for [module])     | NORMAL     |
| **mod_hmh**      | Handmade Hero commands  | schedule [tz timezone]      | Shows HMH schedule                | NORMAL     |
|                  |                         | time                        | Shows time until next HMH stream  | NORMAL     |
| **mod_imgmacro** | Create Image Macros     | newimg \<template\> \<text\>| Uploads an img macro, returns url | WLIST      |
|                  |                         | img \<id\>                  | Recalls the url for an image by id| WLIST      |
|                  |                         | autoimg                     | Auto-generates an image macro     | WLIST      |
| **mod_info**     | Get info from DDG's API | info \<query\>              | Shows information about the query | WLIST      |
| **mod_karma**    | Tracks ++ and --        | karma                       | Shows your own karma              | NORMAL     |
|                  |                         | karma \<user\>              | Shows \<user\>'s karma            | WLIST      |
|                  |                         | ktop [n]                    | Shows overall top n karma         | ADMIN      |
| **mod_linkinfo** | Expands certain links   | *\<none\>*                  | *N/A*                             |            |
| **mod_markov**   | Says dumb things        | say                         | Force say something (5min cd)     | NORMAL     |
|                  |                         | ask                         | Asks something (shared 5min cd)   | NORMAL     |
|                  |                         | interval \<n\>              | Sets the rate of random messages  | ADMIN      |
|                  |                         | len                         | Sets average sentence length      | ADMIN      |
|                  |                         | status                      | Prints markov status info         | ADMIN      |
| **mod_poll**     | Create polls            | poll+ q? opt0 \| .. \| optn | Create a new poll                 | WLIST      |
|                  |                         | poll- [\#id]                | Close a poll by id (or latest)    | WLIST      |
|                  |                         | poll [\#id]                 | Show poll status                  | WLIST      |
|                  |                         | pall / popen                | Show open polls                   | WLIST      |
|                  |                         | vote [\#id] n               | Vote for option n of a poll       | NORMAL     |
| **mod_psa**      | Add periodic messages   | psa+ \<id\> [args]          | Add a new PSA                     | WLIST      |
|                  |                         | psa- \<id\>                 | Remove a previously added PSA     | WLIST      |
| **mod_quotes**   | Stores quotes           | q  [\#chan] \<n\>           | Shows quote n                     | NORMAL     |
|                  |                         | q+ [\#chan] \<text\>        | Adds a new quote                  | WLIST      |
|                  |                         | q- [\#chan] \<n\>           | Removes a quote                   | WLIST      |
|                  |                         | qfix [#chan] \<n\> \<text\> | Changes the text of quote n       | WLIST      |
|                  |                         | qft  [#chan] \<n\> \<time\> | Changes the timestamp of quote n  | WLIST      |
|                  |                         | ql                          | Shows link to quote gist          | NORMAL     |
|                  |                         | qs [\#chan] \<s\>           | Searches for quotes containing s  | NORMAL     |
|                  |                         | qr [\#chan]                 | Shows a random quote              | NORMAL     |
| **mod_schedule** | Stream schedule info    | sched+ [\#chan] \<schedule\>| Adds a schedule                   | WLIST      |
|                  |                         | sched- [\#chan] \<id\>      | Removes a schedule                | WLIST      |
| **mod_twitch**   | Twitch.tv functions     | fnotify \<on\|off\>         | {En,Dis}ables follower notifier   | ADMIN      |
|                  |                         | uptime                      | Shows stream's uptime             | NORMAL     |
|                  |                         | vod                         | Links to the latest VoD           | NORMAL     |
|                  |                         | vod \<user\>                | Links to \<user\>'s latest VoD    | WLIST      |
|                  |                         | streams                     | Manages stream tracker            | WLIST      |
|                  |                         | title \<msg\>               | Sets stream title [if made editor]| WLIST      |
| **mod_whitelist**| Manages permissions     | wl                          | Shows if you are whitelisted      | NORMAL     |
|                  |                         | wl \<user\>                 | Shows if \<user\> is whitelisted  | WLIST      |
|                  |                         | wl+ \<user\>                | Whitelists \<user\>               | ADMIN      |
|                  |                         | wl- \<user\>                | Unwhitelists \<user\>             | ADMIN      |


