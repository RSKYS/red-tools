#ifndef NM_CORE_C
#define NM_CORE_C

#ifndef NM_AMALGAMATED_BUILD
#define NM_AMALGAMATED_BUILD 1
#endif

#include "nm_core.h"

static App app;

static void fatalf(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void rollback_transaction(void);
static void cleanup_temp(void);
static void cleanup_validation_dir(void);

static void *xmalloc(size_t n) {
	void *p = malloc(n ? n : 1);
	if (!p) {
		fputs("\033[31m\033[1m[ERROR]\033[0m Out of memory.\n", stderr);
		exit(1);
	}
	return p;
}

static void *xrealloc(void *p, size_t n) {
	void *q = realloc(p, n ? n : 1);
	if (!q) {
		free(p);
		fputs("\033[31m\033[1m[ERROR]\033[0m Out of memory.\n", stderr);
		exit(1);
	}
	return q;
}

static char *xstrdup(const char *s) {
	if (!s) {
		errno = EINVAL;
		fatalf("Internal error: attempted to duplicate a null string.");
	}
	size_t n = strlen(s) + 1;
	char *p = xmalloc(n);
	memcpy(p, s, n);
	return p;
}

static int str_copy(char *dst, size_t cap, const char *src) {
	if (dst && cap != 0) dst[0] = '\0';
	if (!dst || !src || cap == 0) {
		errno = EINVAL;
		return -1;
	}
	size_t n = strlen(src);
	if (n >= cap) {
		errno = ENAMETOOLONG;
		dst[0] = '\0';
		return -1;
	}
	memcpy(dst, src, n + 1);
	return 0;
}

static int str_printf(char *dst, size_t cap, const char *fmt, ...) {
	if (dst && cap != 0) dst[0] = '\0';
	if (!dst || !fmt || cap == 0) {
		errno = EINVAL;
		return -1;
	}
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, cap, fmt, ap);
	va_end(ap);
	if (n < 0) {
		dst[0] = '\0';
		return -1;
	}
	if ((size_t)n >= cap) {
		errno = ENAMETOOLONG;
		dst[0] = '\0';
		return -1;
	}
	return 0;
}

static const char *env_default(const char *name, const char *fallback) {
	if (!name || !*name) return fallback;
	const char *value = getenv(name);
	return value && *value ? value : fallback;
}

NM_DIAGNOSTIC_PUSH
NM_DIAGNOSTIC_IGNORE("-Wformat-nonliteral")
static void vlog_message(FILE *out, const char *color, const char *label,
						 const char *fmt, va_list ap) {
	if (!out || !color || !label || !fmt) return;

	char hook_message[2048];
	va_list hook_args;
	va_copy(hook_args, ap);
	int hook_length = vsnprintf(hook_message, sizeof(hook_message), fmt, hook_args);
	va_end(hook_args);
	if (hook_length < 0) {
		(void)str_copy(hook_message, sizeof(hook_message), "<log formatting failed>");
	}

	NmLogLevel level = NM_LOG_LEVEL_INFO;
	if (strcmp(label, "SUCCESS") == 0) level = NM_LOG_LEVEL_SUCCESS;
	else if (strcmp(label, "WARNING") == 0) level = NM_LOG_LEVEL_WARNING;
	else if (strcmp(label, "ERROR") == 0) level = NM_LOG_LEVEL_ERROR;
	NM_LOG_HOOK(level, label, hook_message);

	fprintf(out, "%s\033[1m[%s]\033[0m ", color, label);
	vfprintf(out, fmt, ap);
	fputc('\n', out);
	fflush(out);
}
NM_DIAGNOSTIC_POP

static void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_success(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void log_info(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vlog_message(stdout, "\033[34m", "INFO", fmt, ap);
	va_end(ap);
}

static void log_success(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vlog_message(stdout, "\033[32m", "SUCCESS", fmt, ap);
	va_end(ap);
}

static void log_warn(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vlog_message(stderr, "\033[33m", "WARNING", fmt, ap);
	va_end(ap);
}

static void log_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vlog_message(stderr, "\033[31m", "ERROR", fmt, ap);
	va_end(ap);
}

static void fatalf(const char *fmt, ...) {
	cleanup_temp();
	cleanup_validation_dir();
	va_list ap;
	va_start(ap, fmt);
	vlog_message(stderr, "\033[31m", "ERROR", fmt, ap);
	va_end(ap);
	if (app.transaction_active && !app.rollback_done) rollback_transaction();
	exit(1);
}

static void buffer_init(Buffer *b) {
	if (!b) return;
	memset(b, 0, sizeof(*b));
}

static void buffer_free(Buffer *b) {
	if (!b) return;
	free(b->data);
	memset(b, 0, sizeof(*b));
}

static int buffer_reserve(Buffer *b, size_t extra) {
	if (!b || b->len > b->cap) {
		errno = EINVAL;
		return -1;
	}
	if (extra > SIZE_MAX - b->len - 1) {
		errno = EOVERFLOW;
		return -1;
	}
	size_t needed = b->len + extra + 1;
	if (needed <= b->cap) return 0;
	size_t cap = b->cap ? b->cap : 256;
	while (cap < needed) {
		if (cap > SIZE_MAX / 2) {
			cap = needed;
			break;
		}
		cap *= 2;
	}
	b->data = xrealloc(b->data, cap);
	b->cap = cap;
	return 0;
}

static int buffer_append_n(Buffer *b, const char *s, size_t n) {
	if (!b || (!s && n != 0)) {
		errno = EINVAL;
		return -1;
	}
	if (buffer_reserve(b, n) < 0) return -1;
	if (n != 0) memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
	return 0;
}

static int buffer_append(Buffer *b, const char *s) {
	if (!s) {
		errno = EINVAL;
		return -1;
	}
	return buffer_append_n(b, s, strlen(s));
}

static int buffer_printf(Buffer *b, const char *fmt, ...) {
	if (!b || !fmt) {
		errno = EINVAL;
		return -1;
	}
	va_list ap, copy;
	va_start(ap, fmt);
	va_copy(copy, ap);
	int n = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if (n < 0 || buffer_reserve(b, (size_t)n) < 0) {
		va_end(ap);
		return -1;
	}
	int written = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
	va_end(ap);
	if (written != n) {
		errno = EIO;
		return -1;
	}
	b->len += (size_t)n;
	return 0;
}

static void strvec_init(StrVec *v) {
	if (!v) return;
	memset(v, 0, sizeof(*v));
}

static void strvec_free(StrVec *v) {
	if (!v) return;
	for (size_t i = 0; i < v->len; ++i) free(v->items[i]);
	free(v->items);
	memset(v, 0, sizeof(*v));
}

static void strvec_push(StrVec *v, const char *s) {
	if (!v || !s) fatalf("Internal error: invalid string-vector append.");
	if (v->len == v->cap) {
		if (v->cap > SIZE_MAX / 2 ||
			(v->cap ? v->cap * 2 : 16) > SIZE_MAX / sizeof(*v->items))
			fatalf("String-vector capacity overflow.");
		size_t cap = v->cap ? v->cap * 2 : 16;
		v->items = xrealloc(v->items, cap * sizeof(*v->items));
		v->cap = cap;
	}
	v->items[v->len++] = xstrdup(s);
}

static intS strvec_contains(const StrVec *v, const char *s) {
	if (!v || !s) return 0;
	for (size_t i = 0; i < v->len; ++i)
		if (strcmp(v->items[i], s) == 0) return 1;
	return 0;
}

static intS read_input(char *dst, size_t cap) {
	if (!dst || cap < 2 || cap > (size_t)INT_MAX) {
		errno = EINVAL;
		return 0;
	}
	if (!fgets(dst, (int)cap, stdin)) {
		check_interrupted();
		return 0;
	}
	check_interrupted();
	size_t n = strlen(dst);
	if (n && dst[n - 1] == '\n') dst[--n] = '\0';
	else if (n == cap - 1) {
		int c;
		while ((c = getchar()) != '\n' && c != EOF) {}
	}
	if (n && dst[n - 1] == '\r') dst[n - 1] = '\0';
	return 1;
}

static intS confirm(const char *prompt, intS default_yes) {
	char answer[64];
	for (;;) {
		printf("%s [%s]: ", prompt, default_yes ? "Y/n" : "y/N");
		fflush(stdout);
		if (!read_input(answer, sizeof(answer))) {
			putchar('\n');
			return 0;
		}
		if (!*answer) return default_yes;
		if (!strcmp(answer, "y") || !strcmp(answer, "Y") ||
			!strcmp(answer, "yes") || !strcmp(answer, "YES") ||
			!strcmp(answer, "Yes")) return 1;
		if (!strcmp(answer, "n") || !strcmp(answer, "N") ||
			!strcmp(answer, "no") || !strcmp(answer, "NO") ||
			!strcmp(answer, "No")) return 0;
		log_warn("Please answer yes or no.");
	}
}

static char *trim_in_place(char *s) {
	if (!s) return NULL;
	while (isspace((unsigned char)*s)) ++s;
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) --end;
	*end = '\0';
	return s;
}

static int count_char(const char *s, char wanted) {
	if (!s) return 0;
	int n = 0;
	for (; *s; ++s) if (*s == wanted) ++n;
	return n;
}

static intS starts_with(const char *s, const char *prefix) {
	if (!s || !prefix) return 0;
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

static intS ends_with(const char *s, const char *suffix) {
	if (!s || !suffix) return 0;
	size_t ns = strlen(s), nx = strlen(suffix);
	return ns >= nx && memcmp(s + ns - nx, suffix, nx) == 0;
}

static void lower_ascii(char *s) {
	if (!s) return;
	for (; *s; ++s) *s = (char)tolower((unsigned char)*s);
}

static uint32_t posix_cksum_bytes(const unsigned char *data, size_t len) {
	static const uint32_t table[256] = {
		UINT32_C(0x00000000), UINT32_C(0x04C11DB7), UINT32_C(0x09823B6E), UINT32_C(0x0D4326D9),
		UINT32_C(0x130476DC), UINT32_C(0x17C56B6B), UINT32_C(0x1A864DB2), UINT32_C(0x1E475005),
		UINT32_C(0x2608EDB8), UINT32_C(0x22C9F00F), UINT32_C(0x2F8AD6D6), UINT32_C(0x2B4BCB61),
		UINT32_C(0x350C9B64), UINT32_C(0x31CD86D3), UINT32_C(0x3C8EA00A), UINT32_C(0x384FBDBD),
		UINT32_C(0x4C11DB70), UINT32_C(0x48D0C6C7), UINT32_C(0x4593E01E), UINT32_C(0x4152FDA9),
		UINT32_C(0x5F15ADAC), UINT32_C(0x5BD4B01B), UINT32_C(0x569796C2), UINT32_C(0x52568B75),
		UINT32_C(0x6A1936C8), UINT32_C(0x6ED82B7F), UINT32_C(0x639B0DA6), UINT32_C(0x675A1011),
		UINT32_C(0x791D4014), UINT32_C(0x7DDC5DA3), UINT32_C(0x709F7B7A), UINT32_C(0x745E66CD),
		UINT32_C(0x9823B6E0), UINT32_C(0x9CE2AB57), UINT32_C(0x91A18D8E), UINT32_C(0x95609039),
		UINT32_C(0x8B27C03C), UINT32_C(0x8FE6DD8B), UINT32_C(0x82A5FB52), UINT32_C(0x8664E6E5),
		UINT32_C(0xBE2B5B58), UINT32_C(0xBAEA46EF), UINT32_C(0xB7A96036), UINT32_C(0xB3687D81),
		UINT32_C(0xAD2F2D84), UINT32_C(0xA9EE3033), UINT32_C(0xA4AD16EA), UINT32_C(0xA06C0B5D),
		UINT32_C(0xD4326D90), UINT32_C(0xD0F37027), UINT32_C(0xDDB056FE), UINT32_C(0xD9714B49),
		UINT32_C(0xC7361B4C), UINT32_C(0xC3F706FB), UINT32_C(0xCEB42022), UINT32_C(0xCA753D95),
		UINT32_C(0xF23A8028), UINT32_C(0xF6FB9D9F), UINT32_C(0xFBB8BB46), UINT32_C(0xFF79A6F1),
		UINT32_C(0xE13EF6F4), UINT32_C(0xE5FFEB43), UINT32_C(0xE8BCCD9A), UINT32_C(0xEC7DD02D),
		UINT32_C(0x34867077), UINT32_C(0x30476DC0), UINT32_C(0x3D044B19), UINT32_C(0x39C556AE),
		UINT32_C(0x278206AB), UINT32_C(0x23431B1C), UINT32_C(0x2E003DC5), UINT32_C(0x2AC12072),
		UINT32_C(0x128E9DCF), UINT32_C(0x164F8078), UINT32_C(0x1B0CA6A1), UINT32_C(0x1FCDBB16),
		UINT32_C(0x018AEB13), UINT32_C(0x054BF6A4), UINT32_C(0x0808D07D), UINT32_C(0x0CC9CDCA),
		UINT32_C(0x7897AB07), UINT32_C(0x7C56B6B0), UINT32_C(0x71159069), UINT32_C(0x75D48DDE),
		UINT32_C(0x6B93DDDB), UINT32_C(0x6F52C06C), UINT32_C(0x6211E6B5), UINT32_C(0x66D0FB02),
		UINT32_C(0x5E9F46BF), UINT32_C(0x5A5E5B08), UINT32_C(0x571D7DD1), UINT32_C(0x53DC6066),
		UINT32_C(0x4D9B3063), UINT32_C(0x495A2DD4), UINT32_C(0x44190B0D), UINT32_C(0x40D816BA),
		UINT32_C(0xACA5C697), UINT32_C(0xA864DB20), UINT32_C(0xA527FDF9), UINT32_C(0xA1E6E04E),
		UINT32_C(0xBFA1B04B), UINT32_C(0xBB60ADFC), UINT32_C(0xB6238B25), UINT32_C(0xB2E29692),
		UINT32_C(0x8AAD2B2F), UINT32_C(0x8E6C3698), UINT32_C(0x832F1041), UINT32_C(0x87EE0DF6),
		UINT32_C(0x99A95DF3), UINT32_C(0x9D684044), UINT32_C(0x902B669D), UINT32_C(0x94EA7B2A),
		UINT32_C(0xE0B41DE7), UINT32_C(0xE4750050), UINT32_C(0xE9362689), UINT32_C(0xEDF73B3E),
		UINT32_C(0xF3B06B3B), UINT32_C(0xF771768C), UINT32_C(0xFA325055), UINT32_C(0xFEF34DE2),
		UINT32_C(0xC6BCF05F), UINT32_C(0xC27DEDE8), UINT32_C(0xCF3ECB31), UINT32_C(0xCBFFD686),
		UINT32_C(0xD5B88683), UINT32_C(0xD1799B34), UINT32_C(0xDC3ABDED), UINT32_C(0xD8FBA05A),
		UINT32_C(0x690CE0EE), UINT32_C(0x6DCDFD59), UINT32_C(0x608EDB80), UINT32_C(0x644FC637),
		UINT32_C(0x7A089632), UINT32_C(0x7EC98B85), UINT32_C(0x738AAD5C), UINT32_C(0x774BB0EB),
		UINT32_C(0x4F040D56), UINT32_C(0x4BC510E1), UINT32_C(0x46863638), UINT32_C(0x42472B8F),
		UINT32_C(0x5C007B8A), UINT32_C(0x58C1663D), UINT32_C(0x558240E4), UINT32_C(0x51435D53),
		UINT32_C(0x251D3B9E), UINT32_C(0x21DC2629), UINT32_C(0x2C9F00F0), UINT32_C(0x285E1D47),
		UINT32_C(0x36194D42), UINT32_C(0x32D850F5), UINT32_C(0x3F9B762C), UINT32_C(0x3B5A6B9B),
		UINT32_C(0x0315D626), UINT32_C(0x07D4CB91), UINT32_C(0x0A97ED48), UINT32_C(0x0E56F0FF),
		UINT32_C(0x1011A0FA), UINT32_C(0x14D0BD4D), UINT32_C(0x19939B94), UINT32_C(0x1D528623),
		UINT32_C(0xF12F560E), UINT32_C(0xF5EE4BB9), UINT32_C(0xF8AD6D60), UINT32_C(0xFC6C70D7),
		UINT32_C(0xE22B20D2), UINT32_C(0xE6EA3D65), UINT32_C(0xEBA91BBC), UINT32_C(0xEF68060B),
		UINT32_C(0xD727BBB6), UINT32_C(0xD3E6A601), UINT32_C(0xDEA580D8), UINT32_C(0xDA649D6F),
		UINT32_C(0xC423CD6A), UINT32_C(0xC0E2D0DD), UINT32_C(0xCDA1F604), UINT32_C(0xC960EBB3),
		UINT32_C(0xBD3E8D7E), UINT32_C(0xB9FF90C9), UINT32_C(0xB4BCB610), UINT32_C(0xB07DABA7),
		UINT32_C(0xAE3AFBA2), UINT32_C(0xAAFBE615), UINT32_C(0xA7B8C0CC), UINT32_C(0xA379DD7B),
		UINT32_C(0x9B3660C6), UINT32_C(0x9FF77D71), UINT32_C(0x92B45BA8), UINT32_C(0x9675461F),
		UINT32_C(0x8832161A), UINT32_C(0x8CF30BAD), UINT32_C(0x81B02D74), UINT32_C(0x857130C3),
		UINT32_C(0x5D8A9099), UINT32_C(0x594B8D2E), UINT32_C(0x5408ABF7), UINT32_C(0x50C9B640),
		UINT32_C(0x4E8EE645), UINT32_C(0x4A4FFBF2), UINT32_C(0x470CDD2B), UINT32_C(0x43CDC09C),
		UINT32_C(0x7B827D21), UINT32_C(0x7F436096), UINT32_C(0x7200464F), UINT32_C(0x76C15BF8),
		UINT32_C(0x68860BFD), UINT32_C(0x6C47164A), UINT32_C(0x61043093), UINT32_C(0x65C52D24),
		UINT32_C(0x119B4BE9), UINT32_C(0x155A565E), UINT32_C(0x18197087), UINT32_C(0x1CD86D30),
		UINT32_C(0x029F3D35), UINT32_C(0x065E2082), UINT32_C(0x0B1D065B), UINT32_C(0x0FDC1BEC),
		UINT32_C(0x3793A651), UINT32_C(0x3352BBE6), UINT32_C(0x3E119D3F), UINT32_C(0x3AD08088),
		UINT32_C(0x2497D08D), UINT32_C(0x2056CD3A), UINT32_C(0x2D15EBE3), UINT32_C(0x29D4F654),
		UINT32_C(0xC5A92679), UINT32_C(0xC1683BCE), UINT32_C(0xCC2B1D17), UINT32_C(0xC8EA00A0),
		UINT32_C(0xD6AD50A5), UINT32_C(0xD26C4D12), UINT32_C(0xDF2F6BCB), UINT32_C(0xDBEE767C),
		UINT32_C(0xE3A1CBC1), UINT32_C(0xE760D676), UINT32_C(0xEA23F0AF), UINT32_C(0xEEE2ED18),
		UINT32_C(0xF0A5BD1D), UINT32_C(0xF464A0AA), UINT32_C(0xF9278673), UINT32_C(0xFDE69BC4),
		UINT32_C(0x89B8FD09), UINT32_C(0x8D79E0BE), UINT32_C(0x803AC667), UINT32_C(0x84FBDBD0),
		UINT32_C(0x9ABC8BD5), UINT32_C(0x9E7D9662), UINT32_C(0x933EB0BB), UINT32_C(0x97FFAD0C),
		UINT32_C(0xAFB010B1), UINT32_C(0xAB710D06), UINT32_C(0xA6322BDF), UINT32_C(0xA2F33668),
		UINT32_C(0xBCB4666D), UINT32_C(0xB8757BDA), UINT32_C(0xB5365D03), UINT32_C(0xB1F740B4),
	};
	if (!data && len != 0) {
		errno = EINVAL;
		return 0;
	}
	uint32_t crc = 0;
	for (size_t i = 0; i < len; ++i)
		crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & UINT32_C(0xff)];
	size_t n = len;
	while (n) {
		crc = (crc << 8) ^ table[((crc >> 24) ^ (n & UINT32_C(0xff))) & UINT32_C(0xff)];
		n >>= 8;
	}
	return ~crc;
}

#endif
