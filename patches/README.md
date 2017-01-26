This is a hacky patch for [libircclient-1.9](https://sourceforge.net/projects/libircclient/)
 to allow it to parse [IRCv3 tags](http://ircv3.net/specs/core/message-tags-3.2.html).
 
These tags are used by twitch's IRC among others to relay extra info on messages.

This hack puts the tag string in params[-1] in all the callbacks.
After applying the patch, the low version returned from irc_get_version will be
0x1b07 (ibot), so you can use that to tell if accessing params[-1] is safe or not.

insobot will do this version check and run fine with an unpatched lib, just
tags won't be available.

I Imagine a better solution would need to be made if this were to be upstreamed.

BTW libircclient is LGPL, so this patch is too I guess.
