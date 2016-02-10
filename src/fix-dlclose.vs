/* without this mod_markov refuses to unload on dlclose, breaking hot-reloading */
{ global: irc_mod_ctx; local: *; };
