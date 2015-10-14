#include <libircclient.h>
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
#include "config.h"

/* Current Info:

	* ^ is reserved as a start symbol, $ as a end-of-sentence symbol

	* Uses 2 words for each key, 1 for the value.
	  e.g. "This is a sentence" gets parsed as:
	  [^ ^]        -> this,
	  [^ this]     -> is,
	  [this is]    -> a,
	  [is a]       -> sentence,
	  [a sentence] -> $

	* Each unique word is only stored once in the word_storage list,
	  the chains use pointers into word_storage to refer to the words.
	  
	* The word map takes a string hash from std::Hash<std::string> and maps it
	  to the pointer in word_storage.
	  
	* It hashes recent sentences to try and not repeat things that were recently
	  said.
	  
	* Doesn't send more than one message per 5 seconds.
	  
	* There are some bugs related to the fixing / removing word commands, as
	  well as probably many others.
	  
	* The code is messy, copies way more than is necessary, and uses lots of
	  std:: stuff, please forgive me handmade_dev :)

*/

using namespace std;

struct Chain {
	Chain() = default;
	Chain(const string* p, uint32_t c) : ptr(p), count(c){}
	
	const string* ptr;
	uint32_t count;
};

struct Key {

	Key() : word1(nullptr), word2(nullptr){}
	Key(const string* a, const string* b) : word1(a), word2(b) {}

	const string *word1, *word2;
	
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
		s.append(word1 ? *word1 : "(null)");		
		s.append(", ");
		s.append(word2 ? *word2 : "(null)");
	}
};

list<string> word_storage;
unordered_multimap<size_t, string*> word_map;
multimap<Key, Chain> chains;

// store names to avoid recording them in the chains.
unordered_set<string> irc_names;

// 1 in 200 chance (0.5%) to speak, see \gap command to change at runtime.
size_t msg_chance = 200;

size_t max_chain_len = 15;

hash<string> hash_fn;

time_t last_msg_time = 0;
time_t q_timer = 0;

size_t recent_hashes[256] = {};
size_t rh_index = 0;

// use the c++ random stuff since rand()%X has some bias
static random_device dev;
static default_random_engine rng;
static bool seeded = false;

size_t random_num(size_t limit){
	
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
void add_hash(const string& word){
	recent_hashes[rh_index] = hash_fn(word);
	rh_index = (rh_index + 1) % arrsize(recent_hashes);
}

void fix_narcissism(string nick, string& word){
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

bool lookup_key(const string& a, const string& b, Key& out){
	auto a_p = word_map.equal_range(hash_fn(a)),
	     b_p = word_map.equal_range(hash_fn(b));

	string* tmp;
	
	for(auto i = a_p.first, j = a_p.second; i != j; ++i){
		if(*i->second == a){
			tmp = i->second;
			break;
		}
	}
	
	if(!tmp) return false;
	
	for(auto i = b_p.first, j = b_p.second; i != j; ++i){
		if(*i->second == b){
			out.word1 = tmp;
			out.word2 = i->second;
			return true;
		}
	}
	
	return false;
}

void replace_all(string& str, const string& from, const string& to){
	size_t i = 0;
	while(i = str.find(from, i), i != string::npos){
		str.replace(i, from.size(), to);
		i += to.size();
	}
}

void filter_profanity(string& s){
	replace_all(s, "fuck", "f***");
	replace_all(s, "shit", "s***");
	replace_all(s, "cunt", "c***");
}

string markov_gen(){
	
	Key key;
	if(!lookup_key("^", "^", key)){
		puts("Should never happen: ^ not found in info...");
		return "i am error";
	}
	
	string msg;
	set<Key> used_keys;

	for(int chain_len = 1 + random_num(max_chain_len); chain_len > 0; --chain_len){

		auto p = chains.equal_range(key);
		
		if(used_keys.find(key) != used_keys.end()){
			break;
		}
		used_keys.insert(key);
		
		auto i = p.first, j = p.second;
		bool end_available = false;
		size_t total = 0;

		for(; p.first != p.second; ++p.first){
			total += p.first->second.count;
			if(*p.first->second.ptr == "$"){
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
		
		while(count > 0){
			count -= i->second.count;
			if(count > 0) ++i;
		}
				
		const string& next_word = *i->second.ptr;
				
		if(next_word == "$"){
			break;
		}
		
		if(!msg.empty() && next_word != ","){
			msg.append(1, ' ');
		}
						
		msg.append(next_word);
		
		key = Key(i->first.word2, i->second.ptr);
	}

	filter_profanity(msg);
	
	return msg;
}

string markov_get_punct(){

	size_t val = random_num(100);
	
	if(val < 50) return ".";
	if(val < 75) return "?";
	if(val < 85) return "!";
	if(val < 97) return "...";
	if(val < 98) return "‽";
	if(val < 99) return ". FailFish";
	if(val == 99) return ". Kappa";
}

bool check_dup(const string msg){
	if(find(begin(recent_hashes), end(recent_hashes), hash_fn(msg)) != end(recent_hashes)){
		printf("~~~ Skipping duplicate msg [%s]\n", msg.c_str());
		return true;
	}
	return false;
}

void irc_send_ratelimited(irc_session_t* s, const char* chan, const string& msg){
	time_t now = time(0);
	
	if(now - last_msg_time < 5){
		puts("Cancelling msg send: too soon.");
	} else {
		irc_cmd_msg(s, chan, msg.c_str());
		last_msg_time = now;
	}
}

bool markov_gen_formatted(string& msg){
	int val = random_num(15);
	int num_sentences = val < 10 ? 1
	                  : val < 14 ? 2
	                  : 3
	                  ;
	
	while(num_sentences--){
		string s;
		int attempts = 0;
		
		do {
			s = markov_gen();
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

void markov_send(irc_session_t* s, const char* chan){
	string msg;
	if(!markov_gen_formatted(msg)) return;

	printf("!!! Sending msg: [%s]\n", msg.c_str());
	irc_send_ratelimited(s, chan, msg);
}

void markov_pose_q(irc_session_t* s, const char* chan){
	string msg = "Q: ";
	if(!markov_gen_formatted(msg)) return;
	msg.back() = '?';
		
	printf("!!! Sending Q: [%s]\n", msg.c_str());
	irc_send_ratelimited(s, chan, msg);
}

void markov_reply(irc_session_t* s, const char* chan, const char* nick){

	// don't always reply
	if(random_num(3) == 0){
		puts("chooing not to reply.");
		return;
	}

	string msg = "@";
	msg.append(nick).append(1, ' ');
	if(!markov_gen_formatted(msg)) return;
	
	printf("!!! Sending reply: [%s]\n", msg.c_str());
	irc_send_ratelimited(s, chan, msg);
}

string* lookup_or_add_word(const string& s){
	bool found = false;
	auto p = word_map.equal_range(hash_fn(s));
	
	for(auto i = p.first, j = p.second; i != j; ++i){
		if(*i->second == s){
			return i->second;
		}
	}
	
	word_storage.push_back(s);
	string* ptr = &word_storage.back();

	word_map.emplace(hash_fn(s), ptr);
	return ptr;
}

bool markov_unlink(const string& a, const string& b){

	string *a_ptr = lookup_or_add_word(a),
	       *b_ptr = lookup_or_add_word(b);

	bool deleted = false;

	for(auto i = chains.begin(), j = chains.end(); i != j; /**/){
		if(i->first.word2 == a_ptr && i->second.ptr == b_ptr){
			chains.erase(i++);
			deleted = true;
		} else {
			++i;
		}
	}
		
	return deleted;
}

//FIXME: this can create orphan words if their only link was to the removed word.
void markov_remove(const char* word){

	string* ptr = lookup_or_add_word(word);

	for(auto i = chains.begin(), j = chains.end(); i != j; /**/){
		if(i->first.word1 == ptr || i->first.word2 == ptr || i->second.ptr == ptr){
			chains.erase(i++);
		} else {
			++i;
		}
	}
}

void markov_fix(const string& word, const char* fix){
	string* fix_ptr = lookup_or_add_word(word);
	*fix_ptr = fix;
	
	auto p = word_map.equal_range(hash_fn(word));
	for(; p.first != p.second; ++p.first){
		if(p.first->second == fix_ptr){
			word_map.erase(p.first);
			break;
		}
	}
	
	word_map.emplace(hash_fn(*fix_ptr), fix_ptr);
}

extern "C" void markov_write(const char* file);

extern "C" void markov_add_name(const char* name){

	// keep our own name for strange 3rd person sentences...
	if(strcasecmp(name, BOT_NAME) == 0) return;

	auto p = irc_names.insert(name);
	if(p.second){
		printf("Adding name [%s]\n", name);
	}
}

extern "C" void markov_del_name(const char* name){
	//printf("Removing name [%s]\n", name);
	//irc_names.erase(name);
}

void add_chain(string (words)[3]){
	fprintf(
		stderr,
		"Adding [%s, %s] -> [%s]\n",
		words[0].c_str(),
		words[1].c_str(),
		words[2].c_str()
	);

	string* ptrs[3];

	ptrs[0] = lookup_or_add_word(words[0]);
	ptrs[1] = lookup_or_add_word(words[1]);
	ptrs[2] = lookup_or_add_word(words[2]);
	
	Key key(ptrs[0], ptrs[1]);
	auto p = chains.equal_range(key);
	bool found = false;
	
	for(auto i = p.first, j = p.second; i != j; ++i){
		if(*(i->second.ptr) == *ptrs[2]){
			i->second.count++;
			found = true;
			break;
		}
	}
	
	if(!found) chains.emplace(key, Chain(ptrs[2], 1));
};

// remove annoying words that occur due to removing punctuation from emoticons
bool blacklisted(const string& word){
	for(auto* w : { "p", "d", "b", "o", "-p", "-d", "-b", "-o" }){
		if(word == w){
			return true;
		}
	}
	
	return false;
}

bool parse_cmd(const string& msg, string cmd, string* args, size_t num_args){
	cmd.insert(0, 1, '\\');
	if(msg.find(cmd) != 0) return false;
	
	size_t space_idx = cmd.size();
	
	for(size_t i = 0; i < num_args; ++i){
		size_t new_idx = msg.find(" ", space_idx);
		if(new_idx != string::npos){
			args[i] = string(msg, space_idx, new_idx);
			space_idx = new_idx;
		} else {
			return false;
		}
	}
	
	return true;
}

extern "C"
void markov_recv(irc_session_t* s, const char* chan, const char* nick, const char* m){
	bool quiet = false;
	string msg(m);
	
	std::transform(msg.begin(), msg.end(), msg.begin(), ::tolower);
	
	if(!msg.empty() && msg[0] == '!'){
		puts("Skipping command.");
		return;
	}
	
	// skip messages that contain urls
	if(msg.find("http://") != string::npos
	|| msg.find("https://") != string::npos
	|| msg.find("www.") != string::npos){
		puts("Skipping sentence with URL.");
		return;
	}
	
	for(size_t i = msg.find('.'); i != string::npos; i = msg.find('.', i+1)){
		for(size_t j = 1; j < 5; ++j){
			if(i+j > msg.size()){
				break;
			}
			if(msg[i+j] == '/'){
				puts("Found more obscure URL in msg, skipping.");
				return;
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
		irc_cmd_msg(s, chan, response.c_str());
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
		irc_cmd_msg(s, chan, response.c_str());
		return;
	}


	// manual save of dict
	if(strcasecmp(nick, BOT_OWNER) == 0
	&& msg.compare("\\save") == 0){
		string filename = "chains." + to_string(time(0)) + ".txt";
		markov_write(filename.c_str());
		irc_cmd_msg(s, chan, "@" BOT_OWNER " k.");
		return;
	}

	// remove something from dict
	if(strcasecmp(nick, BOT_OWNER) == 0
	&& msg.size() > 4
	&& msg.find("\\rm") == 0){
		markov_remove(msg.c_str() + 4);
		irc_cmd_msg(s, chan, "@" BOT_OWNER " k.");
		return;
	}
	
	if(strcasecmp(nick, BOT_OWNER) == 0
	&& msg.size() > 4
	&& msg.find("\\ne") == 0){
		if(markov_unlink(msg.c_str() + 4, "$")){
			irc_cmd_msg(s, chan, "@" BOT_OWNER " k.");
		} else {
			irc_cmd_msg(s, chan, "@" BOT_OWNER " nuh-uh.");
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
			irc_cmd_msg(s, chan, "@" BOT_OWNER " k.");
		} else {
			irc_cmd_msg(s, chan, "@" BOT_OWNER " nuh-uh.");
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
			irc_cmd_msg(s, chan, "@" BOT_OWNER " k.");
		} else {
			irc_cmd_msg(s, chan, "@" BOT_OWNER " nuh-uh.");
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
		markov_pose_q(s, chan);
		return;
	}
	
	// force speak
	if(msg.compare("\\say") == 0
	&& (strcasecmp(nick, BOT_OWNER) == 0 || time(0) - q_timer > 300)){
		q_timer = time(0);
		markov_send(s, chan);
		return;
	}
	
	// ask q's during prestream
	if(strcasecmp(chan, "#handmade_hero") == 0
	&& strcasecmp(nick, "hmh_bot") == 0
	&& msg.find("prestream q&a.") == 0
	&& random_num(2) == 0){
		markov_pose_q(s, chan);
		quiet = true;
	}
	
	// speak when we are spoken to
	if(!msg.empty() && (msg.find("@" BOT_NAME) != string::npos
	|| msg.find(BOT_NAME ":") == 0)){
		markov_reply(s, chan, nick);
		quiet = true;
	}
	
	// spammy
	if(strcasecmp(nick, "hmh_bot") == 0){
		return;
	}
	
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
	
	// add the name of the list of names to avoid recording
	markov_add_name(nick);
			
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
		int len = word.size();
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
	
	// send a message on a random chance
	if(!quiet && random_num(msg_chance) == 0){
		markov_send(s, chan);
	}
}

extern "C" void markov_read(const char* file){

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
				c.ptr = lookup_or_add_word(word);
			}
			count = !count;
		}
	}
	
	printf("Loaded %zu entries from chains.txt\n", word_map.size());
	
	for(auto& p : chains){
		if(*p.second.ptr != "$"){
			Key key(p.first.word2, p.second.ptr);

			auto it = chains.find(key);

			if(it == chains.end()){
				printf(
					"WARN: fixing orphaned pair [%s, %s]\n", 
					p.first.word2->c_str(),
					p.second.ptr->c_str()
				);
				p.second.ptr = lookup_or_add_word("$");
			}
		}
	}
}

extern "C" void markov_write(const char* file){

	ofstream out(file);
	string line;
	Key key;
	
	for(auto pair : chains){
		if(pair.first != key){
			key = pair.first;

			if(!line.empty()){
				out << line << endl;
			}
			line.clear();
			line.append(*key.word1).append(1, ' ').append(*key.word2);
		}
		line.append(1, ' ')
		    .append(*pair.second.ptr)
		    .append(1, ' ')
		    .append(to_string(pair.second.count));
	}
	out << line << endl;
}


