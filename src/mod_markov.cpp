#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <random>
#include <vector>
#include <climits>
#include <cassert>
#include <list>
#include <experimental/optional>
#include <regex.h>
#include <ext/stdio_filebuf.h>
#include "config.h"
#include "module.h"

/* Current Info:

	* ^ is reserved as a start symbol, $ as a end-of-sentence symbol

	* Uses 2 words for each key, 1 for the value.
	  e.g. "This is a sentence" gets parsed as:
	  [^ ^]        -> this,
	  [^ this]     -> is,
	  [this is]    -> a,
	  [is a]       -> sentence,
	  [a sentence] -> $

	* Each unique word is only stored once in the word_storage vector,
	  the chains use indices into word_storage to refer to the words.
	  
	* The word map takes a string hash from std::Hash<std::string> and maps it
	  to the associated index of word_storage.
	  
	* It hashes recent sentences to try and not repeat things that were recently
	  said.
	  
	* Doesn't send more than one message per 5 seconds.
	  
	* There are some bugs related to the fixing / removing word commands, as
	  well as probably many others.
	  
	* The code is messy, copies way more than is necessary, and uses lots of
	  std:: stuff, please forgive me handmade_dev :)

*/

using namespace std;
using namespace std::experimental;

static const IRCCoreCtx* ctx;

static vector<string> word_storage;
static unordered_multimap<size_t, uint16_t> word_map;

static string& get_word(uint32_t i){
	return word_storage[i];
}

struct Chain {
	Chain() = default;
	Chain(uint16_t i, uint16_t c) : idx(i), count(c){}
	
	uint16_t idx;
	uint16_t count;
};

#define BAD_KEY USHRT_MAX

struct Key {

	Key() : word1(BAD_KEY), word2(BAD_KEY){}
	Key(uint16_t a, uint16_t b) : word1(a), word2(b) {}

	uint16_t word1, word2;
	
	bool operator<(const Key& k) const {
		return tie(word1, word2) < tie(k.word1, k.word2);
	}

	bool operator==(const Key& k) const {
		return tie(word1, word2) == tie(k.word1, k.word2);
	}

	bool operator!=(const Key& k) const {
		return !(*this == k);
	}
	
	string str() const {
		string s;
		s.append(word1 != BAD_KEY ? get_word(word1) : "(null)");		
		s.append(", ");
		s.append(word2 != BAD_KEY ? get_word(word2) : "(null)");
		return s;
	}
};

static multimap<Key, Chain> chains;

// store names to avoid recording them in the chains.
static unordered_set<string> irc_names;

// 1 in 200 chance (0.5%) to speak, see \gap command to change at runtime.
static size_t msg_chance = 200;

static size_t max_chain_len = 15;

static hash<string> hash_fn;

static time_t last_msg_time = 0;
static time_t q_timer = 0;

static size_t recent_hashes[256] = {};
static size_t rh_index = 0;

// use the c++ random stuff since rand()%X has some bias
static random_device dev;
static default_random_engine rng;
static bool seeded = false;

static size_t random_num(size_t limit){
	
	if(!seeded){
		seeded = true;
		rng.seed(dev());
	}
	
	uniform_int_distribution<size_t> dist(0, max<ssize_t>(0, limit - 1));
	
	return dist(rng);
}

// get size of array
template<class T, size_t N>
static constexpr size_t arrsize(const T (&arr)[N]){
	return N;
}

// add the hash of a recently said sentence to avoid repetitions.
static void add_hash(const string& word){
	recent_hashes[rh_index] = hash_fn(word);
	rh_index = (rh_index + 1) % arrsize(recent_hashes);
}

static void fix_narcissism(string nick, string& word){
	nick.append(" is");
	
	string replacement;
	size_t val = random_num(5);
	
	switch(val){
		case 0: replacement = " stupid"; break;
		case 1: replacement = " a fool"; break;
		case 2: replacement = " dumb"; break;
		case 3: replacement = " lame"; break;
		case 4: replacement = " worse than oop"; break;
	}
	
	if(word.find(nick) != string::npos){
		word = nick + replacement;
	}
}

static uint32_t lookup_or_add_word(const string& s){
	auto p = word_map.equal_range(hash_fn(s));
	
	for(auto i = p.first, j = p.second; i != j; ++i){
		if(get_word(i->second) == s){
			return i->second;
		}
	}
	
	word_storage.push_back(s);
	uint32_t idx = word_storage.size() - 1;

	word_map.emplace(hash_fn(s), idx);
	return idx;
}

optional<Key> lookup_key(const string& a, const string& b){

	Key key;

	auto a_p = word_map.equal_range(hash_fn(a)),
	     b_p = word_map.equal_range(hash_fn(b));
	
	for(auto i = a_p.first, j = a_p.second; i != j; ++i){
		if(get_word(i->second) == a){
			key.word1 = i->second;
			break;
		}
	}
	
	if(key.word1 == BAD_KEY) return nullopt;
	
	for(auto i = b_p.first, j = b_p.second; i != j; ++i){
		if(get_word(i->second) == b){
			key.word2 = i->second;
		}
	}
	
	if(key.word2 == BAD_KEY) return nullopt;

	if(chains.find(key) != chains.end()){
		return make_optional(key);
	} else {
		return nullopt;
	}
}

static void replace_all(string& str, const string& from, const string& to){
	size_t i = 0;
	while(i = str.find(from, i), i != string::npos){
		str.replace(i, from.size(), to);
		i += to.size();
	}
}

static void filter_profanity(string& s){
	replace_all(s, "fuck", "f***");
	replace_all(s, "shit", "s***");
	replace_all(s, "cunt", "c***");
}


string markov_gen(optional<Key> opt_key = nullopt){
	
	string msg;
	set<Key> used_keys;
	Key key;

	if(opt_key){
		key = *opt_key;
		msg.append(get_word(key.word1)).append(1, ' ').append(get_word(key.word2));
	} else {
		if(!(opt_key = lookup_key("^", "^"))){
			puts("Should never happen: key not found in info...");
			return "i am error";
		}
		key = *opt_key;
	}

	for(int chain_len = 1 + random_num(max_chain_len); chain_len > 0; --chain_len){

		auto p = chains.equal_range(key);
		
		if(used_keys.find(key) != used_keys.end()){
			break;
		}
		used_keys.insert(key);
		
		auto i = p.first;
		bool end_available = false;
		size_t total = 0;

		for(; p.first != p.second; ++p.first){
			total += p.first->second.count;
			if(get_word(p.first->second.idx) == "$"){
				end_available = true;
			}
		}
		
		//printf("total: %zu\n", total);
		
		if(chain_len == 1){
			if(!end_available){
				++chain_len;
			} else {
				break;
			}
		}
		
		if(i == chains.end()){
			printf("WARN: should never happen: i == end, [%s]\n", key.str().c_str());
			return "i am error";
		}
		
		ssize_t count = 0;
		
		if(total == 0){
			printf("WARN: should never happen: total == 0 [%s]\n", key.str().c_str());
			return msg.empty() ? "i am error" : msg;
		} else {
			count = random_num(total);
		}
		
		while((count -= i->second.count) >= 0){
			++i;
		}
				
		const string& next_word = get_word(i->second.idx);
				
		if(next_word == "$"){
			break;
		}
		
		if(!msg.empty() && next_word != ","){
			msg.append(1, ' ');
		}
						
		msg.append(next_word);
		
		key = Key(i->first.word2, i->second.idx);
	}

	filter_profanity(msg);
	
	return msg;
}

static string markov_get_punct(){

	size_t val = random_num(100);
	
	if(val < 60) return ".";
	if(val < 75) return "?";
	if(val < 85) return "!";
	if(val < 97) return "...";
	if(val < 98) return "‽";
	if(val < 99) return ". FailFish";
	
	return ". Kappa";
}

static bool check_dup(const string msg){
	if(find(begin(recent_hashes), end(recent_hashes), hash_fn(msg)) != end(recent_hashes)){
		printf("~~~ Skipping duplicate msg [%s]\n", msg.c_str());
		return true;
	}
	return false;
}

static void irc_send_ratelimited(const char* chan, const string& msg){
	time_t now = time(0);
	
	if(strcmp(chan, "#" BOT_OWNER) != 0 && (now - last_msg_time) < 5){
		puts("Cancelling msg send: too soon.");
	} else {
		ctx->send_msg(chan, "%s", msg.c_str());
		last_msg_time = now;
	}
}

bool markov_gen_formatted(string& msg, optional<Key> key = nullopt){
	int val = random_num(15);
	int num_sentences = val < 10 ? 1
	                  : val < 14 ? 2
	                  : 3
	                  ;

	int key_sentence = random_num(num_sentences);
	
	while(num_sentences--){
		string s;
		int attempts = 0;
		
		do {
			optional<Key> k = num_sentences == key_sentence ? key : nullopt;
			s = markov_gen(k);

			if(!s.empty() && s[0] == ','){
				s.erase(0, 2);
			}
		} while(attempts++ < 5 && check_dup(s));

		if(attempts >= 5){
			puts("Couldn't get a good message, giving up.");
			return false;
		}
		
		add_hash(s);
		s[0] = ::toupper(s[0]);
		msg.append(s);
		
		if(num_sentences) msg.append(markov_get_punct()).append(1, ' ');
	}

	msg.append(markov_get_punct());
	
	return true;
}

static void markov_send(const char* chan){
	string msg;
	if(!markov_gen_formatted(msg)) return;

	printf("!!! Sending msg: [%s]\n", msg.c_str());
	irc_send_ratelimited(chan, msg);
}

static void markov_pose_q(const char* chan){
	string msg = "Q: ";
	if(!markov_gen_formatted(msg)) return;
	msg.back() = '?';
		
	printf("!!! Sending Q: [%s]\n", msg.c_str());
	irc_send_ratelimited(chan, msg);
}

static void markov_reply(const char* chan, const char* nick){

	// don't always reply
	if(random_num(3) == 0){
		puts("** Choosing not to reply.");
		return;
	}

	string msg = "@";
	msg.append(nick).append(1, ' ');
	if(!markov_gen_formatted(msg)) return;
	
	printf("!!! Sending reply: [%s]\n", msg.c_str());
	irc_send_ratelimited(chan, msg);
}

static bool markov_unlink(const string& a, const string& b){

	uint32_t a_idx = lookup_or_add_word(a),
	         b_idx = lookup_or_add_word(b);

	bool deleted = false;

	for(auto i = chains.begin(), j = chains.end(); i != j; /**/){
		if(i->first.word2 == a_idx && i->second.idx == b_idx){
			chains.erase(i++);
			deleted = true;
		} else {
			++i;
		}
	}
		
	return deleted;
}

//FIXME: this can create orphan words if their only link was to the removed word.
static void markov_remove(const char* word){

	uint32_t idx = lookup_or_add_word(word);

	for(auto i = chains.begin(), j = chains.end(); i != j; /**/){
		if(i->first.word1 == idx || i->first.word2 == idx || i->second.idx == idx){
			chains.erase(i++);
		} else {
			++i;
		}
	}
}

static void markov_fix(const string& word, const char* fix){
	uint32_t fix_idx = lookup_or_add_word(word);
	get_word(fix_idx) = fix;
	
	auto p = word_map.equal_range(hash_fn(word));
	for(; p.first != p.second; ++p.first){
		if(p.first->second == fix_idx){
			word_map.erase(p.first);
			break;
		}
	}
	
	word_map.emplace(hash_fn(word), fix_idx);
}

static void markov_add_name(const char* chan, const char* name){

	// keep our own name for strange 3rd person sentences...
	if(strcasecmp(name, ctx->get_username()) == 0) return;

	auto p = irc_names.insert(name);
	if(p.second){
		printf("Adding name [%s]\n", name);
	}
}

static void add_chain(string (words)[3]){
	fprintf(
		stderr,
		"Adding [%s, %s] -> [%s]\n",
		words[0].c_str(),
		words[1].c_str(),
		words[2].c_str()
	);

	uint32_t idx[3] = {
		lookup_or_add_word(words[0]),
		lookup_or_add_word(words[1]),
		lookup_or_add_word(words[2])
	};
	
	Key key(idx[0], idx[1]);
	auto p = chains.equal_range(key);
	bool found = false;
	
	for(auto i = p.first, j = p.second; i != j; ++i){
		if(i->second.idx == idx[2]){
			i->second.count++;
			found = true;
			break;
		}
	}
	
	if(!found) chains.emplace(key, Chain(idx[2], 1));
};

// remove annoying words that occur due to removing punctuation from emoticons
static bool blacklisted(const string& word){
	for(auto* w : { "p", "d", "b", "o", "-p", "-d", "-b", "-o" }){
		if(word == w){
			return true;
		}
	}
	
	return false;
}

static bool markov_write(FILE* file);


static regex_t opinion_regex;

static void markov_msg(const char* chan, const char* nick, const char* m){
	bool quiet = false,
	     skip_chain = false;

	string msg(m);
	
	std::transform(msg.begin(), msg.end(), msg.begin(), ::tolower);
	
	if(!msg.empty() && msg[0] == '!'){
		puts("Skipping command.");
		skip_chain = true;
	}
	
	// skip messages that contain urls
	if(msg.find("http://") != string::npos
	|| msg.find("https://") != string::npos
	|| msg.find("www.") != string::npos){
		puts("Skipping sentence with URL.");
		skip_chain = true;
	}
	
	for(size_t i = msg.find('.'); i != string::npos; i = msg.find('.', i+1)){
		for(size_t j = 1; j < 5; ++j){
			if(i+j > msg.size()){
				break;
			}
			if(msg[i+j] == '/'){
				puts("Found more obscure URL in msg, skipping.");
				skip_chain = true;
			}
		}
	}
		
	// change msg chance
	if(strcasecmp(nick, BOT_OWNER) == 0
	&& msg.find("\\gap") == 0){
		if(msg.size() > 5){
			char* end;
			msg_chance = max<int>(1, strtol(msg.c_str() + 5, &end, 10));
		}
		string response = "@" BOT_OWNER " gap=" + to_string(msg_chance);
		ctx->send_msg(chan, "%s", response.c_str());
		return;
	}
	
	// change max chain length
	if(strcasecmp(nick, BOT_OWNER) == 0
	&& msg.find("\\max") == 0){
		if(msg.size() > 5){
			char* end;
			max_chain_len = max<int>(1, strtol(msg.c_str() + 5, &end, 10));
		}
		string response = "@" BOT_OWNER " max=" + to_string(max_chain_len);
		ctx->send_msg(chan, "%s", response.c_str());
		return;
	}


	// manual save of dict
	if(strcasecmp(nick, BOT_OWNER) == 0
	&& msg.compare("\\save") == 0){
		string filename = "chains." + to_string(time(0)) + ".txt";
		FILE* f = fopen(filename.c_str(), "wb");
		markov_write(f);
		fclose(f);
		ctx->send_msg(chan, "%s", "@" BOT_OWNER " k.");
		return;
	}

	// remove something from dict
	if(strcasecmp(nick, BOT_OWNER) == 0
	&& msg.size() > 4
	&& msg.find("\\rm") == 0){
		markov_remove(msg.c_str() + 4);
		ctx->send_msg(chan, "%s", "@" BOT_OWNER " k.");
		return;
	}
	
	if(strcasecmp(nick, BOT_OWNER) == 0
	&& msg.size() > 4
	&& msg.find("\\ne") == 0){
		if(markov_unlink(msg.c_str() + 4, "$")){
			ctx->send_msg(chan, "%s", "@" BOT_OWNER " k.");
		} else {
			ctx->send_msg(chan, "%s", "@" BOT_OWNER " nuh-uh.");
		}
		return;
	}
	
	if(strcasecmp(nick, BOT_OWNER) == 0
	&& msg.size() > 8
	&& msg.find("\\unlink") == 0){
		size_t space = msg.find(" ", 8);
		if(space != string::npos && markov_unlink(
			string(msg.c_str() + 8, msg.c_str() + space), msg.c_str() + space + 1
		)){
			ctx->send_msg(chan, "%s", "@" BOT_OWNER " k.");
		} else {
			ctx->send_msg(chan, "%s", "@" BOT_OWNER " nuh-uh.");
		}
		return;
	}
	
	if(strcasecmp(nick, BOT_OWNER) == 0
	&& msg.size() > 5
	&& msg.find("\\fix") == 0){
		size_t space = msg.find(" ", 5);
		if(space != string::npos){
			markov_fix(
				string(msg.c_str() + 5, msg.c_str() + space),
				msg.c_str() + space + 1
			);
			ctx->send_msg(chan, "%s", "@" BOT_OWNER " k.");
		} else {
			ctx->send_msg(chan, "%s", "@" BOT_OWNER " nuh-uh.");
		}
		return;
	}

	/*
	// ask a question during the q&a
	if(strcasecmp(chan, "#handmade_hero") == 0
	&& strcasecmp(nick, "cmuratori") == 0
	&& msg.compare("!qa") == 0){
		usleep(1000000);
		markov_pose_q(s, chan);
		quiet = true;
	}
	*/

	// ask a question
	if(msg.compare("\\ask") == 0
	&& (strcasecmp(nick, BOT_OWNER) == 0 || time(0) - q_timer > 300)){
		q_timer = time(0);
		markov_pose_q(chan);
		return;
	}
	
	// force speak
	if(msg.compare("\\say") == 0
	&& (strcasecmp(nick, BOT_OWNER) == 0 || time(0) - q_timer > 300)){
		q_timer = time(0);
		markov_send(chan);
		return;
	}

	if(msg == "\\status"){
		char buffer[256];
		snprintf(buffer, sizeof(buffer), "Greetings human. I know %zu unique words, and %zu word links. http://github.com/insofaras/insobot", word_storage.size(), chains.size());
		irc_send_ratelimited(chan, buffer);
		return;
	}
	
	// ask q's during prestream
	if(strcasecmp(chan, "#handmade_hero") == 0
	&& strcasecmp(nick, "hmh_bot") == 0
	&& msg.find("prestream q&a.") == 0
	&& random_num(2) == 0){
		markov_pose_q(chan);
		quiet = true;
	}
	
	// speak when we are spoken to
	regmatch_t matches[4] = {};

	const char* bot_name = ctx->get_username();
	size_t      bot_name_len = strlen(bot_name);
	const char* name_pats[] = { "@%s", "%s:", "%s," };
	char        name_buf[256];
	bool        found_name = false;

	assert(bot_name_len + 2 < sizeof(name_buf));
	for(size_t i = 0; i < arrsize(name_pats); ++i){
		snprintf(name_buf, sizeof(name_buf), name_pats[i], bot_name);
		if(msg.find(name_buf) != string::npos){
			found_name = true;
			break;
		}
	}

	if(found_name){
		if(
			msg.find("?") != string::npos &&
			regexec(&opinion_regex, msg.c_str(), 4, matches, 0) == 0
		){
			if(random_num(3) == 0){
				puts("** Choosing to not reply.");
				return;
			}

			string hotword(msg.c_str() + matches[3].rm_so, msg.c_str() + matches[3].rm_so);
			if(hotword == "me") hotword = "you";

			const char* link_words[] = { "is", "are", "can", "should", "could", "will", "might", "may" };
			for(size_t i = 0; i < arrsize(link_words); ++i){
				size_t r = random_num(arrsize(link_words));

				const char* tmp = link_words[r];
				link_words[r] = link_words[i];
				link_words[i] = tmp;
			}

			optional<Key> k;
			for(size_t i = 0; i < arrsize(link_words); ++i){
				k = lookup_key(hotword, link_words[i]);
				if(k) break;
			}

			string reply("@");
			reply.append(nick).append(1, ' ');
			markov_gen_formatted(reply, k);
			irc_send_ratelimited(chan, reply);
		} else {
			markov_reply(chan, nick);
		}

		quiet = true;
	}

	// spammy
	if(strcasecmp(nick, "hmh_bot") == 0){
		skip_chain = true;
	}

	// add the name of the list of names to avoid recording
	markov_add_name(nullptr, nick);

	if(*m == '\\') skip_chain = true;
	
	if(!skip_chain){
	
		// remove name at start
		auto space_idx = msg.find(' ');
		if (space_idx != string::npos && space_idx > 1 
		&& (msg[0] == '@' || msg[space_idx-1] == ':')){
			msg.erase(0, space_idx);
		}
	
		// split sentences at . ! ?
		replace_all(msg, "$", "@");
		replace_all(msg, ". ", " $ ");
		replace_all(msg, "! ", " $ ");
		replace_all(msg, "? ", " $ ");
	
		// treat comma as a separate word
		replace_all(msg, ",", " , ");

		// remove non-ascii chars
		msg.erase(
			remove_if(msg.begin(), msg.end(), [](unsigned char c){
				return c < ' ' || c >= 127;
			}),
			msg.end()
		);
	
		// remove most punctuation (by changing it to a space)
		std::transform(msg.begin(), msg.end(), msg.begin(), [](char c){
			if(strchr(".!?@:;`^(){}[]\"", c) != nullptr){
				return ' ';
			} else {
				return c;
			}
		});

		// condense multiple consequitive spaces + trim
		replace_all(msg, "  ", " ");
		size_t pos;
		if((pos = msg.find_first_not_of(" ")) != string::npos){
			msg.erase(0, pos);
		}
		if((pos = msg.find_last_not_of(" ")) != string::npos){
			msg.erase(pos + 1);
		}

		// store the hash to avoid repeating people
		add_hash(msg);

		// turn self-promotion into self-deprecation
		fix_narcissism(nick, msg);

		stringstream stream(msg);
		string words[3] = { "^", "^", "" };
		auto& word = words[2];

		while(getline(stream, word, ' ')){
			if(word.empty() || blacklisted(word)) continue;
			if(word.size() > 28) word = "***";
		
			// skip empty sentences
			if(word == "$" && words[1] == "^") continue;

			// skip dups
			if(word == words[1] && words[1] == words[0]) continue;
		
			// trim out ++ and -- after names
			size_t len = word.size();
			if (len > 2 && (word.find("++") == len - 2 || word.find("--") == len - 2)){
				len -= 2;
			}
		
			// skip names
			auto it = irc_names.find(string(word.begin(), word.begin() + len));
			if(it != irc_names.end()){
				printf("Skipping name [%s]\n", it->c_str());
				continue;
			}

			// add these 3 words as a new chain
			add_chain(words);

			// prepare for the next chain, if this ended a sentence start anew
			if(word == "$"){
				words[0] = "^";
				words[1] = "^";
			} else {
				words[0] = move(words[1]);
				words[1] = move(words[2]);
			}
		}

		// add the final ending marker to the chain
		word = "$";
		if(words[1] != "^") add_chain(words);

	}
	
	// send a message on a random chance
	if(!quiet && random_num(msg_chance) == 0){
		markov_send(chan);
	}
}

static void markov_read(const char* file){

	word_storage.clear();
	chains.clear();

	ifstream in(file);
	string line;
	
	while(getline(in, line)){
		stringstream str(line);
		string word, k1, k2;

		getline(str, k1, ' ');
		getline(str, k2, ' ');

		Key key(lookup_or_add_word(k1), lookup_or_add_word(k2));
		
		Chain c;
		bool count = false;
		while(getline(str, word, ' ')){
			if(count){
				c.count = stoul(word);
				chains.emplace(key, c);
			} else {
				c.idx = lookup_or_add_word(word);
			}
			count = !count;
		}
	}
	
	printf("Loaded %zu words, %zu chains.\n", word_storage.size(), chains.size());
	
	for(auto& p : chains){
		if(get_word(p.second.idx) != "$"){
			Key key(p.first.word2, p.second.idx);

			auto it = chains.find(key);

			if(it == chains.end()){
				printf(
					"WARN: fixing orphaned pair [%s, %s]\n", 
					get_word(p.first.word2).c_str(),
					get_word(p.second.idx).c_str()
				);
				p.second.idx = lookup_or_add_word("$");
			}
		}
	}
}

static bool markov_write(FILE* file){

	__gnu_cxx::stdio_filebuf<char> buf(file, std::ios::out);

	ostream out(&buf);
	string line;
	Key key;

	printf("Saving %zu words, %zu chains.\n", word_storage.size(), chains.size());

	for(auto pair : chains){
		if(pair.first != key){
			key = pair.first;

			if(!line.empty()){
				out << line << endl;
			}
			line.clear();
			line.append(get_word(key.word1)).append(1, ' ').append(get_word(key.word2));
		}
		line.append(1, ' ')
		    .append(get_word(pair.second.idx))
		    .append(1, ' ')
		    .append(to_string(pair.second.count));
	}
	out << line << endl;

	return true;
}

static bool markov_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	regcomp(
		&opinion_regex,
 		"^.*(do you (like|hate|think about|feel about)|opinion [a-z]+|thoughts [a-z]+|feelings [a-z]+|your take on) ([^?[:space:]]+).*$",
		REG_EXTENDED | REG_ICASE
	);

	markov_read(ctx->get_datafile());
	return true;
}

// no designated initializer in C++ :(
extern "C" const IRCModuleCtx irc_mod_ctx = {
	"markov",
	"Occasionally say something incomprehensible",
	nullptr, // cmds
	100,     // prio
	IRC_MOD_DEFAULT,
	&markov_init,
	nullptr, // quit
	nullptr, // connect
	&markov_msg,
	&markov_add_name,
	nullptr, // part
	nullptr, // nick
	nullptr, // cmd
	&markov_write
};

void is_dlclose_actually_working() __attribute__((destructor));
void is_dlclose_actually_working(){
	puts("Yes, dlclose is actually working.");
}
