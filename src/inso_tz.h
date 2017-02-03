#if !defined(INSO_TZ_H) && !defined(INSO_IMPL)
#define INSO_TZ_H
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

bool tz_abbr2off(const char* abbr, int* offset_out);

static inline char* tz_push(const char* tz){
	char* oldtz = getenv("TZ");
	if(oldtz) oldtz = strdup(oldtz);

	setenv("TZ", tz, 1);
	tzset();

	return oldtz;
}

static inline void tz_pop(char* oldtz){
	if(oldtz){
		setenv("TZ", oldtz, 1);
		free(oldtz);
	} else {
		unsetenv("TZ");
	}
	tzset();
}

#endif

#ifdef INSO_IMPL
#undef INSO_IMPL
#include "inso_utils.h"

const struct {
	const char* abbr;
	int offset;
} tz_abbrs[] = {
	{ "A", 0 },
	{ "ACDT", 630 },
	{ "ACST", 570 },
	{ "ACT", -300 },
	{ "ADT", -180 },
	{ "AEDT", 660 },
	{ "AEST", 600 },
	{ "AFT", 270 },
	{ "AKDT", -480 },
	{ "AKST", -540 },
	{ "AMST", -180 },
	{ "AMT", -240 },
	{ "ART", -180 },
	{ "AST", -240 },
	{ "AWDT", 540 },
	{ "AWST", 480 },
	{ "AZOST", -60 },
	{ "AZT", 240 },
	{ "B", 120 },
	{ "BDT", 480 },
	{ "BIOT", 360 },
	{ "BIT", -720 },
	{ "BOT", -240 },
	{ "BRST", -120 },
	{ "BRT", -180 },
	{ "BST", 60 },
	{ "BTT", 360 },
	{ "C", 180 },
	{ "CAT", 120 },
	{ "CCT", 390 },
	{ "CDT", -300 },
	{ "CEDT", 120 },
	{ "CEST", 120 },
	{ "CET", 60 },
	{ "CHADT", 825 },
	{ "CHAST", 765 },
	{ "CHOT", 480 },
	{ "ChST", 600 },
	{ "CHUT", 600 },
	{ "CIST", -480 },
	{ "CIT", 480 },
	{ "CKT", -600 },
	{ "CLST", -180 },
	{ "CLT", -240 },
	{ "COST", -240 },
	{ "COT", -300 },
	{ "CST", -360 },
	{ "CT", 480 },
	{ "CVT", -60 },
	{ "CWST", 525 },
	{ "CXT", 420 },
	{ "D", 240 },
	{ "DAVT", 420 },
	{ "DDUT", 600 },
	{ "DFT", 60 },
	{ "E", 300 },
	{ "EASST", -300 },
	{ "EAST", -360 },
	{ "EAT", 180 },
	{ "ECT", -300 },
	{ "EDT", -240 },
	{ "EEDT", 180 },
	{ "EEST", 180 },
	{ "EET", 120 },
	{ "EGST", 0 },
	{ "EGT", -60 },
	{ "EIT", 540 },
	{ "EST", -300 },
	{ "F", 360 },
	{ "FET", 180 },
	{ "FJT", 720 },
	{ "FKST", -180 },
	{ "FKT", -240 },
	{ "FNT", -120 },
	{ "G", 420 },
	{ "GALT", -360 },
	{ "GAMT", -540 },
	{ "GET", 240 },
	{ "GFT", -180 },
	{ "GILT", 720 },
	{ "GIT", -540 },
	{ "GMT", 0 },
	{ "GST", -120 },
	{ "GYT", -240 },
	{ "H", 480 },
	{ "HADT", -540 },
	{ "HAEC", 120 },
	{ "HAST", -600 },
	{ "HKT", 480 },
	{ "HMT", 300 },
	{ "HOVT", 420 },
	{ "HST", -600 },
	{ "I", 540 },
	{ "ICT", 420 },
	{ "IDT", 180 },
	{ "IOT", 180 },
	{ "IRDT", 270 },
	{ "IRKT", 480 },
	{ "IRST", 210 },
	{ "IST", 330 },
	{ "JST", 540 },
	{ "K", 600 },
	{ "KGT", 360 },
	{ "KOST", 660 },
	{ "KRAT", 420 },
	{ "KST", 540 },
	{ "L", 660 },
	{ "LHST", 630 },
	{ "LINT", 840 },
	{ "M", 720 },
	{ "MAGT", 720 },
	{ "MART", -510 },
	{ "MAWT", 300 },
	{ "MDT", -360 },
	{ "MET", 60 },
	{ "MEST", 120 },
	{ "MHT", 720 },
	{ "MIST", 660 },
	{ "MIT", -510 },
	{ "MMT", 390 },
	{ "MSK", 180 },
	{ "MST", -420 },
	{ "MUT", 240 },
	{ "MVT", 300 },
	{ "MYT", 480 },
	{ "N", -60 },
	{ "NCT", 660 },
	{ "NDT", -90 },
	{ "NFT", 690 },
	{ "NPT", 345 },
	{ "NST", -150 },
	{ "NT", -150 },
	{ "NUT", -660 },
	{ "NZDT", 780 },
	{ "NZST", 720 },
	{ "O", -120 },
	{ "OMST", 360 },
	{ "ORAT", 300 },
	{ "P", -180 },
	{ "PDT", -420 },
	{ "PET", -300 },
	{ "PETT", 720 },
	{ "PGT", 600 },
	{ "PHOT", 780 },
	{ "PKT", 300 },
	{ "PMDT", -120 },
	{ "PMST", -180 },
	{ "PONT", 660 },
	{ "PST", -480 },
	{ "PYST", -180 },
	{ "PYT", -240 },
	{ "R", -300 },
	{ "RET", 240 },
	{ "ROTT", -180 },
	{ "S", -360 },
	{ "SAMT", 240 },
	{ "SAST", 120 },
	{ "SBT", 660 },
	{ "SCT", 240 },
	{ "SGT", 480 },
	{ "SLST", 330 },
	{ "SRET", 660 },
	{ "SRT", -180 },
	{ "SST", 480 },
	{ "SYOT", 180 },
	{ "T", -420 },
	{ "TAHT", -600 },
	{ "THA", 420 },
	{ "TFT", 300 },
	{ "TJT", 300 },
	{ "TKT", 780 },
	{ "TLT", 540 },
	{ "TMT", 300 },
	{ "TOT", 780 },
	{ "TVT", 720 },
	{ "U", -480 },
	{ "UCT", 0 },
	{ "ULAT", 480 },
	{ "USZ1", 120 },
	{ "UTC", 0 },
	{ "UYST", -120 },
	{ "UYT", -180 },
	{ "UZT", 300 },
	{ "VET", -210 },
	{ "V", -540 },
	{ "VLAT", 600 },
	{ "VOLT", 240 },
	{ "VOST", 360 },
	{ "VUT", 660 },
	{ "W", -600 },
	{ "WAKT", 720 },
	{ "WAST", 120 },
	{ "WAT", 60 },
	{ "WEDT", 60 },
	{ "WEST", 60 },
	{ "WET", 0 },
	{ "WIT", 420 },
	{ "WST", 480 },
	{ "X", -660 },
	{ "Y", -720 },
	{ "YAKT", 540 },
	{ "YEKT", 300 },
	{ "Z", 0 },
};

bool tz_abbr2off(const char* abbr, int* offset){
	for(size_t i = 0; i < ARRAY_SIZE(tz_abbrs); ++i){
		if(strcasecmp(abbr, tz_abbrs[i].abbr) == 0){
			if(offset) *offset = tz_abbrs[i].offset;
			return true;
		}
	}
	return false;
}

#endif
