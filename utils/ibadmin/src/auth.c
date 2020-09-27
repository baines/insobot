#include "ibadmin.h"
#include <envz.h>
#include <time.h>
#include <crypt.h>

#define AUTH_FILE "/var/www/ibadmin/auth.txt"
#define HASHLEN 32

extern void sha256 (uint8_t hash[static HASHLEN], const void* input, size_t len);

static struct crypt_data crypt_ctx;

static void sha256_hmac(uint8_t out[static HASHLEN], const char* plaintext, const char* key)
{
	uint8_t buf[HASHLEN*3];

	uint8_t* ko = buf;
	uint8_t* ki = buf + HASHLEN;
	uint8_t* ht = buf + HASHLEN * 2;

	sha256(ko, key, strlen(key));
	memcpy(ki, ko, HASHLEN);

	for(int i = 0; i < HASHLEN; ++i) {
		ki[i] ^= 0x36;
		ko[i] ^= 0x5c;
	}

	sha256(ht, plaintext, strlen(plaintext));
	sha256(ki, ki, HASHLEN * 2);
	sha256(out, ko, HASHLEN * 2);
}

static void hexify(char out[static HASHLEN*2+1], const uint8_t in[static HASHLEN])
{
	static const char hex[] = "0123456789abcdef";
	char* p = out;

	for(int i = 0; i < HASHLEN; ++i) {
		*p++ = hex[in[i] >> 4];
		*p++ = hex[in[i] & 15];
	}

	*p = '\0';
}

static void unhexify(uint8_t out[static HASHLEN], const char in[static HASHLEN*2])
{
	static const uint8_t lookup[32] = {
		[17] = 1, 2, 3, 4, 5, 6, 7, 8, 9,
		[1] = 10, 11, 12, 13, 14, 15,
	};

	uint8_t* p = out;

	for(int i = 0; i < HASHLEN*2; i += 2) {
		*p++ = (lookup[in[i+0] & 0x1f] << 4)
			 | (lookup[in[i+1] & 0x1f]);
	}
}

static bool check_hmac(const char* hash, const char* user, const char* time, const char* hmac)
{
	const char* secret = util_getenv("IBADMIN_SECRET");

	char* key  _auto_free_ = xsprintf("%s:%s", secret, hash);
	char* text _auto_free_ = xsprintf("%s:%s:", user, time);

	uint8_t hmac_real[HASHLEN];
	uint8_t hmac_user[HASHLEN];

	unhexify(hmac_user, hmac);
	sha256_hmac(hmac_real, text, key);

	return memcmp(hmac_real, hmac_user, HASHLEN) == 0;
}

static char* read_user_line(char buf[static 512], const char* username, char** line)
{
	FILE* f _auto_close_ = fopen(AUTH_FILE, "a+");
	if(!f) {
		util_exit(500);
	}

	while(fgets(buf, 512, f)) {
		*line = buf;

		char* user = strsep(line, " ");
		if(!user) {
			util_exit(500);
		}

		if(strcmp(user, username) == 0) {
			return user;
		}
	}

	return NULL;
}

bool auth_cookie_read (struct auth_user* output)
{
	char* cookie _auto_free_ = NULL;
	{
		const char* _cookie_header = getenv("HTTP_COOKIE");
		if(!_cookie_header) {
			return false;
		}

		char* cookie_header _auto_free_ = strdup(_cookie_header);

		while((cookie = strsep(&cookie_header, " ;"))) {
			if(strncmp(cookie, "AUTH=", 5) == 0) {
				cookie = strdup(cookie+5);
				break;
			}
		}

		if(!cookie) {
			return false;
		}
	}

	char* cookie_user = strsep(&cookie, ":");
	char* cookie_time = strsep(&cookie, ":");
	char* cookie_hmac = strsep(&cookie, ":");

	if(!cookie_user || !cookie_time || !cookie_hmac) {
		return false;
	}

	// TODO: convert cookie_time to int, compare with time(0), reject if too old

	char buf[512];
	char* line = buf;

	char* user = read_user_line(buf, cookie_user, &line);
	if(!user) {
		return false;
	}

	char* hash = strsep(&line, " ");
	if(!hash) {
		util_exit(500);
	}

	if(!check_hmac(hash, cookie_user, cookie_time, cookie_hmac)) {
		return false;
	}

	char* chans = strsep(&line, "\n");
	if(!chans) {
		util_exit(500);
	}

	output->user = strdup(user);
	argz_create_sep(chans, ' ', &output->chan_argz, &output->nchans);

	return true;
}

char* auth_cookie_create (const char* username, const char* password)
{
	time_t now = time(0);
	int offset = 0;

	char buf[512];
	char* line = buf;

	char* user = read_user_line(buf, username, &line);
	if(!user) {
		return false;
	}

	char* hash = strsep(&line, " ");
	if(!hash) {
		util_exit(500);
	}

	// check password matches DB
	char* user_hash = crypt_r(password, hash, &crypt_ctx);
	if(!user_hash || strcmp(user_hash, hash) != 0) {
		return NULL;
	}

	const char* secret = util_getenv("IBADMIN_SECRET");
	char* key _auto_free_ = xsprintf("%s:%s", secret, hash);

	char* result = xsprintf(
		"AUTH=%s:%lx:%n|00-|01-|02-|03-|04-|05-|06-|07-|08-|09-|10-|11-|12-|13-|14-|15-",
		username, (long)now,
		&offset
	);

	if(offset <= 0) {
		util_exit(500);
	}

	result[offset] = '\0';
	uint8_t hmac[HASHLEN];

	sha256_hmac(hmac, result + 5, key);
	hexify(result + offset, hmac);

	return result;
}
