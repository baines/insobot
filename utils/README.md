## Utility programs related to insobot

## `ibdox`:

Generates a documentation webpage based on the .help and .help_url properties
listed by each module

## `ibadmin`:

This is an external CGI website that uses mod_extadmin to pass commands to insobot.
Useful for adding aliases / PSAs / timers etc without spamming the chat.

## `schedule_api.c`:

If you want to use the mod_schedule stuff, you can put this simple CGI program
on a server somewhere as an alternative to using github's gists for storage.

## `mod_core_upgrade.c`:

Older versions of insobot had mod_chans and mod_meta, these have since been
combined into mod_core. This program will create data/core.data based on
the contents of data/meta.data and data/chans.data
