
static const char* graphql =
"query($page: Int) {"
" Page(perPage: 1, page: $page) {"
"  media(type: ANIME, popularity_greater: 1000, sort: [ID]) {"
"   id"
"   title {"
"    english,"
"    romaji"
"   },"
"   startDate {"
"    year"
"   }"
"   source,"
"   studios(isMain: true) {"
"    nodes {"
"     name"
"    }"
"   },"
"   characters(role: MAIN) {"
"    nodes {"
"     name {"
"      full"
"     }"
"    }"
"   }"
"  }"
" }"
"}";

static int max_page = 10105;

enum quiz_types {
	Q_YEAR    = (1 << 0),
	Q_STUDIOS = (1 << 1),
	Q_CHARS   = (1 << 2),

	Q_COUNT   = 3
};

static bool trivia_start_anilist(const char* chan, struct TriviaState* ts) {
	bool result = false;

	char* data = NULL;
	CURL* curl = inso_curl_init("https://graphql.anilist.co", &data);

	int page = rand() % max_page;

	char* json;
	asprintf_check(&json, "{ \"query\": \"%s\", \"variables\": { \"page\": %d } }", graphql, page);

	struct curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");

	curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	//curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);

	long rsp = inso_curl_perform(curl, &data);

	curl_slist_free_all(headers);

	if (rsp != 200) {
		printf("anilist bad rsp %ld %s\n", rsp, data);
		goto end;
	}

	struct uj_node* root = uj_parse(data, strlen(data), NULL);
	struct uj_node* media_array = UJ_GET(root, ("data", "Page", "media"), UJ_ARR);

	if(!media_array) {
		printf("no media array\n");
		uj_node_free(root, 1);
		goto end;
	}

	struct uj_node* media = media_array->arr;
	struct uj_node* title_en = UJ_GET(media, ("title", "english"), UJ_STR);
	struct uj_node* title_rj = UJ_GET(media, ("title", "romaji"), UJ_STR);

	char* title;
	if(title_en) {
		title = title_en->str;
	} else if(title_rj) {
		title = title_rj->str;
	} else {
		uj_node_free(root, 1);
		goto end;
	}

	printf("page %d title = [%s]\n", page, title);

	int quiz_mask = 0;

	struct uj_node* year = UJ_GET(media, ("startDate", "year"), UJ_INT);
	if(year) {
		quiz_mask |= Q_YEAR;
	}

	struct uj_node* studios_array = UJ_GET(media, ("studios", "nodes"), UJ_ARR);
	sb(char*) studios = NULL;

	for(size_t i = 0; i < studios_array->len; ++i) {
		struct uj_node* s = UJ_GET(studios_array->arr + i, ("name"), UJ_STR);
		if(s) {
			sb_push(studios, s->str);
		}
	}

	if(sb_count(studios) > 0) {
		quiz_mask |= Q_STUDIOS;
	}

	struct uj_node* chars_array = UJ_GET(media, ("characters", "nodes"), UJ_ARR);
	sb(char*) chars = NULL;

	for(size_t i = 0; i < chars_array->len; ++i) {
		struct uj_node* name = UJ_GET(chars_array->arr + i, ("name", "full"), UJ_STR);
		if(name) {
			sb_push(chars, name->str);
		}
	}

	if(sb_count(chars) > 0) {
		quiz_mask |= Q_CHARS;
	}

	if(quiz_mask == 0) {
		printf("no quiz options...\n");
		goto end;
	}

	int qmax = rand() % __builtin_popcount(quiz_mask);
	int quiz_type;

	if((quiz_mask & Q_YEAR) && rand() % 2 == 0) {
		quiz_type = Q_YEAR;
	} else {
		for(int i = 0; i < Q_COUNT; ++i) {
			int type = (1 << i);
			if((quiz_mask & type) && qmax == 0) {
				quiz_type = type;
				break;
			}

			if(qmax > 0) {
				--qmax;
			}
		}
	}

	if(quiz_type == Q_YEAR) {
		asprintf_check(&ts->question, "In what year was \"%s\" released?", title);
		char year_str[32];
		snprintf(year_str, sizeof(year_str), "%d", (int)year->num);
		argz_add(&ts->answer_argz, &ts->answer_sz, year_str);
	} else if(quiz_type == Q_STUDIOS) {
		asprintf_check(&ts->question, "Which studio produced \"%s\"?", title);
		sb_each(s, studios) {
			argz_add(&ts->answer_argz, &ts->answer_sz, *s);
		}
	} else if(quiz_type == Q_CHARS) {
		asprintf_check(&ts->question, "Name a main character from \"%s\"", title);
		sb_each(c, chars) {
			argz_add(&ts->answer_argz, &ts->answer_sz, *c);
		}
	} else {
		printf("ERM\n");
		goto end;
	}

	ts->hint = time(0) + 30;
	ts->end  = time(0) + 60;
	result   = true;

end:
	curl_easy_cleanup(curl);
	free(json);
	sb_free(data);
	return result;
}
