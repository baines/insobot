#include <stdint.h>
#include <stdlib.h>

static uint8_t b64initialised;

static uint8_t b64indices[256];
void b64init(void)
{
    static const uint8_t lookup[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for(const uint8_t* p = lookup; *p; ++p) {
        b64indices[*p] = p - lookup;
    }
	b64initialised = 1;
}

char* unbase64(const char* _in, size_t len)
{
	if(!b64initialised) {
		b64init();
	}

	const uint8_t* in = (const uint8_t*)_in;
    const uint8_t* end = in + len;
    uint8_t c0, c1, c2, c3;
    uint8_t* output = malloc(((len/4)+1)*3+1);
    uint8_t* out = output;

    while(end - in >= 4) {
        c0 = b64indices[*in++];
        c1 = b64indices[*in++];
        c2 = b64indices[*in++];
        c3 = b64indices[*in++];
        *out++ = (c0 << 2) | (c1 >> 4);
        *out++ = ((c1 & 0xf) << 4) | (c2 >> 2);
        *out++ = ((c2 & 0x3) << 6) | (c3);
    }

    c0 = c1 = c2 = c3 = 0;

    switch(end - in) {
        case 0: goto end;
        case 3: c2 = b64indices[in[2]]; // fall-thru
        case 2: c1 = b64indices[in[1]]; // fall-thru
        case 1: c0 = b64indices[in[0]]; // fall-thru
    }

    *out++ = (c0 << 2) | (c1 >> 4);
    *out++ = ((c1 & 0xf) << 4) | (c2 >> 2);
    *out++ = ((c2 & 0x3) << 6);

end:
    *out = '\0';
    return (char*)output;
}
