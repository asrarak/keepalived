/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        Configuration file parser/reader. Place into the dynamic
 *              data structure representation the conf file representing
 *              the loadbalanced server pool.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include <glob.h>
#include <unistd.h>
#include <libgen.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <linux/version.h>
#include <pwd.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <inttypes.h>

#include "parser.h"
#include "memory.h"
#include "logger.h"
#include "rttables.h"
#include "scheduler.h"
#include "notify.h"
#include "list.h"
#include "bitops.h"
#include "utils.h"

#define DUMP_KEYWORDS	0

#define DEF_LINE_END	"\n"

#define COMMENT_START_CHRS "!#"
#define BOB "{"
#define EOB "}"
#define WHITE_SPACE_STR " \t\f\n\r\v"

typedef struct _defs {
	char *name;
	size_t name_len;
	char *value;
	size_t value_len;
	bool multiline;
	char *(*fn)(void);
} def_t;

/* global vars */
vector_t *keywords;
char *config_id;
const char *WHITE_SPACE = WHITE_SPACE_STR;

/* local vars */
static vector_t *current_keywords;
static FILE *current_stream;
static int sublevel = 0;
static int skip_sublevel = 0;
static list multiline_stack;
static char *buf_extern;
static config_err_t config_err = CONFIG_OK; /* Highest level of config error for --config-test */

/* Parameter definitions */
static list defs;

/* Forward declarations for recursion */
static bool read_line(char *, size_t);

void
report_config_error(config_err_t err, const char *format, ...)
{
	va_list args;

	va_start(args, format);

	if (__test_bit(CONFIG_TEST_BIT, &debug)) {
		vfprintf(stderr, format, args);
		fputc('\n', stderr);

		if (config_err == CONFIG_OK || config_err < err)
			config_err = err;
	}
	else
		vlog_message(LOG_INFO, format, args);

	va_end(args);
}

config_err_t
get_config_status(void)
{
	return config_err;
}

static char *
null_strvec(const vector_t *strvec, size_t index)
{
	if (index - 1 < vector_size(strvec) && index > 0 && vector_slot(strvec, index - 1))
		report_config_error(CONFIG_MISSING_PARAMETER, "*** Configuration line starting `%s` is missing a parameter after keyword `%s` at word position %zu", vector_slot(strvec, 0) ? (char *)vector_slot(strvec, 0) : "***MISSING ***", (char *)vector_slot(strvec, index - 1), index + 1);
	else
		report_config_error(CONFIG_MISSING_PARAMETER, "*** Configuration line starting `%s` is missing a parameter at word position %zu", vector_slot(strvec, 0) ? (char *)vector_slot(strvec, 0) : "***MISSING ***", index + 1);

	exit(KEEPALIVED_EXIT_CONFIG);

	return NULL;
}

static bool
read_int_func(const char *number, int base, const char *msg, const char *info, int *res, int min_val, int max_val, __attribute__((unused)) bool ignore_error)
{
	long val;
	char *endptr;
	char *warn = "";

#ifndef _STRICT_CONFIG_
	if (ignore_error && !__test_bit(CONFIG_TEST_BIT, &debug))
		warn = "WARNING - ";
#endif

	errno = 0;
	val = strtol(number, &endptr, base);
	*res = (int)val;

	if (*endptr)
		report_config_error(CONFIG_INVALID_NUMBER, "%s%s '%s' has invalid number '%s'", warn, msg, info, number);
	else if (errno == ERANGE || val < INT_MIN || val > INT_MAX)
		report_config_error(CONFIG_INVALID_NUMBER, "%s%s '%s' has number '%s' outside integer range", warn, msg, info, number);
	else if (val < min_val || val > max_val)
		report_config_error(CONFIG_INVALID_NUMBER, "%s '%s' has number '%s' outside range [%d, %d]", msg, info, number, min_val, max_val);
	else
		return true;

#ifdef _STRICT_CONFIG_
	return false;
#else
	return ignore_error && val >= min_val && val <= max_val && !__test_bit(CONFIG_TEST_BIT, &debug);
#endif
}

static bool
read_unsigned_func(const char *number, int base, const char *msg, const char *info, unsigned *res, unsigned min_val, unsigned max_val, __attribute__((unused)) bool ignore_error)
{
	unsigned long val;
	char *endptr;
	char *warn = "";
	size_t offset;

#ifndef _STRICT_CONFIG_
	if (ignore_error && !__test_bit(CONFIG_TEST_BIT, &debug))
		warn = "WARNING - ";
#endif

	/* In case the string starts with spaces (even in the configuration this
	 * can be achieved by enclosing the number in quotes - e.g. weight "  -100")
	 * skip any leading whitespace */
	offset = strspn(number, WHITE_SPACE);

	errno = 0;
	val = strtoul(number + offset, &endptr, base);
	*res = (unsigned)val;

	if (number[offset] == '-')
		report_config_error(CONFIG_INVALID_NUMBER, "%s%s '%s' has negative number '%s'", warn, msg, info, number);
	else if (*endptr)
		report_config_error(CONFIG_INVALID_NUMBER, "%s%s '%s' has invalid number '%s'", warn, msg, info, number);
	else if (errno == ERANGE || val > UINT_MAX)
		report_config_error(CONFIG_INVALID_NUMBER, "%s%s '%s' has number '%s' outside unsigned integer range", warn, msg, info, number);
	else if (val < min_val || val > max_val)
		report_config_error(CONFIG_INVALID_NUMBER, "%s '%s' has number '%s' outside range [%u, %u]", msg, info, number, min_val, max_val);
	else
		return true;

#ifdef _STRICT_CONFIG_
	return false;
#else
	return ignore_error && val >= min_val && val <= max_val && !__test_bit(CONFIG_TEST_BIT, &debug);
#endif
}

static bool
read_unsigned64_func(const char *number, int base, const char *msg, const char *info, uint64_t *res, uint64_t min_val, uint64_t max_val, __attribute__((unused)) bool ignore_error)
{
	unsigned long long val;
	char *endptr;
	char *warn = "";
	size_t offset;

#ifndef _STRICT_CONFIG_
	if (ignore_error && !__test_bit(CONFIG_TEST_BIT, &debug))
		warn = "WARNING - ";
#endif

	/* In case the string starts with spaces (even in the configuration this
	 * can be achieved by enclosing the number in quotes - e.g. weight "  -100")
	 * skip any leading whitespace */
	offset = strspn(number, WHITE_SPACE);

	errno = 0;
	val = strtoull(number + offset, &endptr, base);
	*res = (unsigned)val;

	if (number[offset] == '-')
		report_config_error(CONFIG_INVALID_NUMBER, "%s%s '%s' has negative number '%s'", warn, msg, info, number);
	else if (*endptr)
		report_config_error(CONFIG_INVALID_NUMBER, "%s%s '%s' has invalid number '%s'", warn, msg, info, number);
	else if (errno == ERANGE)
		report_config_error(CONFIG_INVALID_NUMBER, "%s%s '%s' has number '%s' outside unsigned 64 bit range", warn, msg, info, number);
	else if (val < min_val || val > max_val)
		report_config_error(CONFIG_INVALID_NUMBER, "%s '%s' has number '%s' outside range [%" PRIu64 ", %" PRIu64 "]", msg, info, number, min_val, max_val);
	else
		return true;

#ifdef _STRICT_CONFIG_
	return false;
#else
	return ignore_error && val >= min_val && val <= max_val && !__test_bit(CONFIG_TEST_BIT, &debug);
#endif
}

static bool
read_double_func(const char *number, const char *msg, const char *info, double *res, double min_val, double max_val, __attribute__((unused)) bool ignore_error)
{
	double val;
	char *endptr;
	char *warn = "";

#ifndef _STRICT_CONFIG_
	if (ignore_error && !__test_bit(CONFIG_TEST_BIT, &debug))
		warn = "WARNING - ";
#endif

	errno = 0;
	val = strtod(number, &endptr);
	*res = val;

	if (*endptr)
		report_config_error(CONFIG_INVALID_NUMBER, "%s%s '%s' has invalid number '%s'", warn, msg, info, number);
	else if (errno == ERANGE)
		report_config_error(CONFIG_INVALID_NUMBER, "%s%s '%s' has number '%s' out of range", warn, msg, info, number);
	else if (val == -HUGE_VAL || val == HUGE_VAL)	/* +/- Inf */
		report_config_error(CONFIG_INVALID_NUMBER, "%s '%s' has infinite number '%s'", msg, info, number);
	else if (!(val <= 0 || val >= 0))	/* NaN */
		report_config_error(CONFIG_INVALID_NUMBER, "%s '%s' has not a number '%s'", msg, info, number);
	else if (val < min_val || val > max_val)
		report_config_error(CONFIG_INVALID_NUMBER, "%s '%s' has number '%s' outside range [%g, %g]", msg, info, number, min_val, max_val);
	else
		return true;

#ifdef _STRICT_CONFIG_
	return false;
#else
	return ignore_error && val >= min_val && val <= max_val && !__test_bit(CONFIG_TEST_BIT, &debug);
#endif
}

bool
read_int(const char *str, int *res, int min_val, int max_val, bool ignore_error)
{
	return read_int_func(str, 10, "Number", str, res, min_val, max_val, ignore_error);
}

bool
read_unsigned(const char *str, unsigned *res, unsigned min_val, unsigned max_val, bool ignore_error)
{
	return read_unsigned_func(str, 10, "Number", str, res, min_val, max_val, ignore_error);
}

bool
read_unsigned64(const char *str, uint64_t *res, uint64_t min_val, uint64_t max_val, bool ignore_error)
{
	return read_unsigned64_func(str, 10, "Number", str, res, min_val, max_val, ignore_error);
}

bool
read_double(const char *str, double *res, double min_val, double max_val, bool ignore_error)
{
	return read_double_func(str, "Number", str, res, min_val, max_val, ignore_error);
}

bool
read_int_strvec(const vector_t *strvec, size_t index, int *res, int min_val, int max_val, bool ignore_error)
{
	return read_int_func(strvec_slot(strvec, index), 10, "Line starting", strvec_slot(strvec, 0), res, min_val, max_val, ignore_error);
}

bool
read_unsigned_strvec(const vector_t *strvec, size_t index, unsigned *res, unsigned min_val, unsigned max_val, bool ignore_error)
{
	return read_unsigned_func(strvec_slot(strvec, index), 10, "Line starting", strvec_slot(strvec, 0), res, min_val, max_val, ignore_error);
}

bool
read_unsigned64_strvec(const vector_t *strvec, size_t index, uint64_t *res, uint64_t min_val, uint64_t max_val, bool ignore_error)
{
	return read_unsigned64_func(strvec_slot(strvec, index), 10, "Line starting", strvec_slot(strvec, 0), res, min_val, max_val, ignore_error);
}

bool
read_double_strvec(const vector_t *strvec, size_t index, double *res, double min_val, double max_val, bool ignore_error)
{
	return read_double_func(strvec_slot(strvec, index), "Line starting", strvec_slot(strvec, 0), res, min_val, max_val, ignore_error);
}

bool
read_unsigned_base_strvec(const vector_t *strvec, size_t index, int base, unsigned *res, unsigned min_val, unsigned max_val, bool ignore_error)
{
	return read_unsigned_func(strvec_slot(strvec, index), base, "Line starting", strvec_slot(strvec, 0), res, min_val, max_val, ignore_error);
}

static void
keyword_alloc(vector_t *keywords_vec, const char *string, void (*handler) (vector_t *), bool active)
{
	keyword_t *keyword;

	vector_alloc_slot(keywords_vec);

	keyword = (keyword_t *) MALLOC(sizeof(keyword_t));
	keyword->string = string;
	keyword->handler = handler;
	keyword->active = active;

	vector_set_slot(keywords_vec, keyword);
}

static void
keyword_alloc_sub(vector_t *keywords_vec, const char *string, void (*handler) (vector_t *))
{
	int i = 0;
	keyword_t *keyword;

	/* fetch last keyword */
	keyword = vector_slot(keywords_vec, vector_size(keywords_vec) - 1);

	/* Don't install subordinate keywords if configuration block inactive */
	if (!keyword->active)
		return;

	/* position to last sub level */
	for (i = 0; i < sublevel; i++)
		keyword = vector_slot(keyword->sub, vector_size(keyword->sub) - 1);

	/* First sub level allocation */
	if (!keyword->sub)
		keyword->sub = vector_alloc();

	/* add new sub keyword */
	keyword_alloc(keyword->sub, string, handler, true);
}

/* Exported helpers */
void
install_sublevel(void)
{
	sublevel++;
}

void
install_sublevel_end(void)
{
	sublevel--;
}

void
install_keyword_root(const char *string, void (*handler) (vector_t *), bool active)
{
	/* If the root keyword is inactive, the handler will still be called,
	 * but with a NULL strvec */
	keyword_alloc(keywords, string, handler, active);
}

void
install_root_end_handler(void (*handler) (void))
{
	keyword_t *keyword;

	/* fetch last keyword */
	keyword = vector_slot(keywords, vector_size(keywords) - 1);

	if (!keyword->active)
		return;

	keyword->sub_close_handler = handler;
}

void
install_keyword(const char *string, void (*handler) (vector_t *))
{
	keyword_alloc_sub(keywords, string, handler);
}

void
install_sublevel_end_handler(void (*handler) (void))
{
	int i = 0;
	keyword_t *keyword;

	/* fetch last keyword */
	keyword = vector_slot(keywords, vector_size(keywords) - 1);

	if (!keyword->active)
		return;

	/* position to last sub level */
	for (i = 0; i < sublevel; i++)
		keyword = vector_slot(keyword->sub, vector_size(keyword->sub) - 1);
	keyword->sub_close_handler = handler;
}

#if DUMP_KEYWORDS
static void
dump_keywords(vector_t *keydump, int level, FILE *fp)
{
	unsigned int i;
	keyword_t *keyword_vec;
	char file_name[21];

	if (!level) {
		snprintf(file_name, sizeof(file_name), "/tmp/keywords.%d", getpid());
		fp = fopen(file_name, "w");
		if (!fp)
			return;
	}

	for (i = 0; i < vector_size(keydump); i++) {
		keyword_vec = vector_slot(keydump, i);
		fprintf(fp, "%*sKeyword : %s (%s)\n", level * 2, "", keyword_vec->string, keyword_vec->active ? "active": "disabled");
		if (keyword_vec->sub)
			dump_keywords(keyword_vec->sub, level + 1, fp);
	}

	if (!level)
		fclose(fp);
}
#endif

static void
free_keywords(vector_t *keywords_vec)
{
	keyword_t *keyword_vec;
	unsigned int i;

	for (i = 0; i < vector_size(keywords_vec); i++) {
		keyword_vec = vector_slot(keywords_vec, i);
		if (keyword_vec->sub)
			free_keywords(keyword_vec->sub);
		FREE(keyword_vec);
	}
	vector_free(keywords_vec);
}

/* Functions used for standard definitions */
static char *
get_cwd(void)
{
	char *dir = MALLOC(PATH_MAX);

	/* Since keepalived doesn't do a chroot(), we don't need to be concerned
	 * about (unreachable) - see getcwd(3) man page. */
	return getcwd(dir, PATH_MAX);
}

static char *
get_instance(void)
{
	char *conf_id = MALLOC(strlen(config_id) + 1);

	strcpy(conf_id, config_id);

	return conf_id;
}

vector_t *
alloc_strvec_quoted_escaped(char *src)
{
	char *token;
	vector_t *strvec;
	char cur_quote = 0;
	char *ofs_op;
	char *op_buf;
	char *ofs, *ofs1;
	char op_char;

	if (!src) {
		if (!buf_extern)
			return NULL;
		src = buf_extern;
	}

	/* Create a vector and alloc each command piece */
	strvec = vector_alloc();
	op_buf = MALLOC(MAXBUF);

	ofs = src;
	while (*ofs) {
		/* Find the next 'word' */
		ofs += strspn(ofs, WHITE_SPACE);
		if (!*ofs || strchr(COMMENT_START_CHRS, *ofs))
			break;

		ofs_op = op_buf;

		while (*ofs) {
			ofs1 = strpbrk(ofs, cur_quote == '"' ? "\"\\" : cur_quote == '\'' ? "'\\" : WHITE_SPACE_STR COMMENT_START_CHRS "'\"\\");

			if (!ofs1) {
				size_t len;
				if (cur_quote) {
					report_config_error(CONFIG_UNMATCHED_QUOTE, "String '%s': missing terminating %c", src, cur_quote);
					goto err_exit;
				}
				strcpy(ofs_op, ofs);
				len =  strlen(ofs);
				ofs += len;
				ofs_op += len;
				break;
			}

			/* Save the wanted text */
			strncpy(ofs_op, ofs, ofs1 - ofs);
			ofs_op += ofs1 - ofs;
			ofs = ofs1;

			if (*ofs == '\\') {
				/* It is a '\' */
				ofs++;

				if (!*ofs) {
					log_message(LOG_INFO, "Missing escape char at end: '%s'", src);
					goto err_exit;
				}

				if (*ofs == 'x' && isxdigit(ofs[1])) {
					op_char = 0;
					ofs++;
					while (isxdigit(*ofs)) {
						op_char <<= 4;
						op_char |= isdigit(*ofs) ? *ofs - '0' : (10 + *ofs - (isupper(*ofs)  ? 'A' : 'a'));
						ofs++;
					}
				}
				else if (*ofs == 'c' && ofs[1]) {
					op_char = *++ofs & 0x1f;	/* Convert to control character */
					ofs++;
				}
				else if (*ofs >= '0' && *ofs <= '7') {
					op_char = *ofs++ - '0';
					if (*ofs >= '0' && *ofs <= '7') {
						op_char <<= 3;
						op_char += *ofs++ - '0';
					}
					if (*ofs >= '0' && *ofs <= '7') {
						op_char <<= 3;
						op_char += *ofs++ - '0';
					}
				}
				else {
					switch (*ofs) {
					case 'a':
						op_char = '\a';
						break;
					case 'b':
						op_char = '\b';
						break;
					case 'E':
						op_char = 0x1b;
						break;
					case 'f':
						op_char = '\f';
						break;
					case 'n':
						op_char = '\n';
						break;
					case 'r':
						op_char = '\r';
						break;
					case 't':
						op_char = '\t';
						break;
					case 'v':
						op_char = '\v';
						break;
					default: /* \"'  */
						op_char = *ofs;
						break;
					}
					ofs++;
				}

				*ofs_op++ = op_char;
				continue;
			}

			if (cur_quote) {
				/* It's the close quote */
				ofs++;
				cur_quote = 0;
				continue;
			}

			if (*ofs == '"' || *ofs == '\'') {
				cur_quote = *ofs++;
				continue;
			}

			break;
		}

		token = MALLOC(ofs_op - op_buf + 1);
		memcpy(token, op_buf, ofs_op - op_buf);
		token[ofs_op - op_buf] = '\0';

		/* Alloc & set the slot */
		vector_alloc_slot(strvec);
		vector_set_slot(strvec, token);
	}

	FREE(op_buf);

	if (!vector_size(strvec)) {
		free_strvec(strvec);
		return NULL;
	}

	return strvec;

err_exit:
	free_strvec(strvec);
	FREE(op_buf);
	return NULL;
}

vector_t *
alloc_strvec_r(char *string)
{
	char *cp, *start, *token;
	size_t str_len;
	vector_t *strvec;

	if (!string)
		return NULL;

	/* Create a vector and alloc each command piece */
	strvec = vector_alloc();

	cp = string;
	while (true) {
		cp += strspn(cp, WHITE_SPACE);
		if (!*cp || strchr(COMMENT_START_CHRS, *cp))
			break;

		start = cp;

		/* Save a quoted string without the ""s as a single string */
		if (*start == '"') {
			start++;
			if (!(cp = strchr(start, '"'))) {
				report_config_error(CONFIG_UNMATCHED_QUOTE, "Unmatched quote: '%s'", string);
				break;
			}
			str_len = (size_t)(cp - start);
			cp++;
		} else {
			cp += strcspn(start, WHITE_SPACE_STR COMMENT_START_CHRS "\"");
			str_len = (size_t)(cp - start);
		}
		token = MALLOC(str_len + 1);
		memcpy(token, start, str_len);
		token[str_len] = '\0';

		/* Alloc & set the slot */
		vector_alloc_slot(strvec);
		vector_set_slot(strvec, token);
	}

	if (!vector_size(strvec)) {
		free_strvec(strvec);
		return NULL;
	}

	return strvec;
}

/* recursive configuration stream handler */
static int kw_level = 0;
static bool
process_stream(vector_t *keywords_vec, int need_bob)
{
	unsigned int i;
	keyword_t *keyword_vec;
	char *str;
	char *buf;
	vector_t *strvec;
	vector_t *prev_keywords = current_keywords;
	current_keywords = keywords_vec;
	int bob_needed = 0;
	bool ret_err = false;
	bool ret;

	buf = MALLOC(MAXBUF);
	while (read_line(buf, MAXBUF)) {
		strvec = alloc_strvec(buf);

		if (!strvec)
			continue;

		str = vector_slot(strvec, 0);

		if (skip_sublevel == -1) {
			/* There wasn't a '{' on the keyword line */
			if (!strcmp(str, BOB)) {
				/* We've got the opening '{' now */
				skip_sublevel = 1;
				need_bob = 0;
				free_strvec(strvec);
				continue;
			}

			/* The skipped keyword doesn't have a {} block, so we no longer want to skip */
			skip_sublevel = 0;
		}
		if (skip_sublevel) {
			for (i = 0; i < vector_size(strvec); i++) {
				str = vector_slot(strvec,i);
				if (!strcmp(str,BOB))
					skip_sublevel++;
				else if (!strcmp(str,EOB)) {
					if (--skip_sublevel == 0)
						break;
				}
			}

			/* If we have reached the outer level of the block and we have
			 * nested keyword level, then we need to return to restore the
			 * next level up of keywords. */
			if (!strcmp(str, EOB) && skip_sublevel == 0 && kw_level > 0) {
				ret_err = true;
				free_strvec(strvec);
				break;
			}

			free_strvec(strvec);
			continue;
		}

		if (need_bob) {
			need_bob = 0;
			if (!strcmp(str, BOB) && kw_level > 0) {
				free_strvec(strvec);
				continue;
			}
			else
				report_config_error(CONFIG_MISSING_BOB, "Missing '%s' at beginning of configuration block", BOB);
		}
		else if (!strcmp(str, BOB)) {
			report_config_error(CONFIG_UNEXPECTED_BOB, "Unexpected '%s' - ignoring", BOB);
			free_strvec(strvec);
			continue;
		}

		if (!strcmp(str, EOB) && kw_level > 0) {
			free_strvec(strvec);
			break;
		}

		for (i = 0; i < vector_size(keywords_vec); i++) {
			keyword_vec = vector_slot(keywords_vec, i);

			if (!strcmp(keyword_vec->string, str)) {
				if (!keyword_vec->active) {
					if (!strcmp(vector_slot(strvec, vector_size(strvec)-1), BOB))
						skip_sublevel = 1;
					else
						skip_sublevel = -1;

					/* Sometimes a process wants to know if another process
					 * has any of a type of configuration. For example, there
					 * is no point starting the VRRP process of there are no
					 * vrrp instances, and so the parent process would be
					 * interested in that. */
					if (keyword_vec->handler)
						(*keyword_vec->handler)(NULL);
				}

				/* There is an inconsistency here. 'static_ipaddress' for example
				 * does not have sub levels, but needs a '{' */
				if (keyword_vec->sub) {
					/* Remove a trailing '{' */
					char *bob = vector_slot(strvec, vector_size(strvec)-1) ;
					if (!strcmp(bob, BOB)) {
						vector_unset(strvec, vector_size(strvec)-1);
						FREE(bob);
						bob_needed = 0;
					}
					else
						bob_needed = 1;
				}

				if (keyword_vec->active && keyword_vec->handler) {
					buf_extern = buf;	/* In case the raw line wants to be accessed */
					(*keyword_vec->handler) (strvec);
				}

				if (keyword_vec->sub) {
					kw_level++;
					ret = process_stream(keyword_vec->sub, bob_needed);
					kw_level--;

					/* We mustn't run any close handler if the block was skipped */
					if (!ret && keyword_vec->active && keyword_vec->sub_close_handler)
						(*keyword_vec->sub_close_handler) ();
				}
				break;
			}
		}

		if (i >= vector_size(keywords_vec))
			report_config_error(CONFIG_UNKNOWN_KEYWORD, "Unknown keyword '%s'", str);

		free_strvec(strvec);
	}

	current_keywords = prev_keywords;
	FREE(buf);
	return ret_err;
}

static bool
read_conf_file(const char *conf_file)
{
	FILE *stream;
	glob_t globbuf;
	size_t i;
	int	res;
	struct stat stb;
	unsigned num_matches = 0;

	globbuf.gl_offs = 0;
	res = glob(conf_file, GLOB_MARK
#if HAVE_DECL_GLOB_BRACE
					| GLOB_BRACE
#endif
						    , NULL, &globbuf);

	if (res) {
		if (res == GLOB_NOMATCH)
			log_message(LOG_INFO, "No config files matched '%s'.", conf_file);
		else
			log_message(LOG_INFO, "Error reading config file(s): glob(\"%s\") returned %d, skipping.", conf_file, res);
		return true;
	}

	for (i = 0; i < globbuf.gl_pathc; i++) {
		if (globbuf.gl_pathv[i][strlen(globbuf.gl_pathv[i])-1] == '/') {
			/* This is a directory - so skip */
			continue;
		}

		log_message(LOG_INFO, "Opening file '%s'.", globbuf.gl_pathv[i]);
		stream = fopen(globbuf.gl_pathv[i], "r");
		if (!stream) {
			log_message(LOG_INFO, "Configuration file '%s' open problem (%s) - skipping"
				       , globbuf.gl_pathv[i], strerror(errno));
			continue;
		}

		/* Make sure what we have opened is a regular file, and not for example a directory or executable */
		if (fstat(fileno(stream), &stb) ||
		    !S_ISREG(stb.st_mode) ||
		    (stb.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
			log_message(LOG_INFO, "Configuration file '%s' is not a regular non-executable file - skipping", globbuf.gl_pathv[i]);
			fclose(stream);
			continue;
		}

		num_matches++;

		current_stream = stream;

		int curdir_fd = -1;
		if (strchr(globbuf.gl_pathv[i], '/')) {
			/* If the filename contains a directory element, change to that directory.
			   The man page open(2) states that fchdir() didn't support O_PATH until Linux 3.5,
			   even though testing on Linux 3.1 shows it appears to work. To be safe, don't
			   use it until Linux 3.5. */
			curdir_fd = open(".", O_RDONLY | O_DIRECTORY
#if HAVE_DECL_O_PATH && LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
								     | O_PATH
#endif
									     );

			char *confpath = strdup(globbuf.gl_pathv[i]);
			dirname(confpath);
			if (chdir(confpath) < 0)
				log_message(LOG_INFO, "chdir(%s) error (%s)", confpath, strerror(errno));
			free(confpath);
		}

		process_stream(current_keywords, 0);
		fclose(stream);

		/* If we changed directory, restore the previous directory */
		if (curdir_fd != -1) {
			if ((res = fchdir(curdir_fd)))
				log_message(LOG_INFO, "Failed to restore previous directory after include");
			close(curdir_fd);
			if (res)
				return true;
		}
	}

	globfree(&globbuf);

	if (skip_sublevel) {
		log_message(LOG_INFO, "WARNING - %d missing '}'(s) in the config file(s)", skip_sublevel);
		skip_sublevel = 0;
	}

	if (!num_matches)
		log_message(LOG_INFO, "No config files matched '%s'.", conf_file);

	return false;
}

bool check_conf_file(const char *conf_file)
{
	glob_t globbuf;
	size_t i;
	bool ret = true;
	int res;
	struct stat stb;
	unsigned num_matches = 0;

	globbuf.gl_offs = 0;
	res = glob(conf_file, GLOB_MARK
#if HAVE_DECL_GLOB_BRACE
					| GLOB_BRACE
#endif
						    , NULL, &globbuf);
	if (res) {
		report_config_error(CONFIG_FILE_NOT_FOUND, "Unable to find configuration file %s (glob returned %d)", conf_file, res);
		return false;
	}

	for (i = 0; i < globbuf.gl_pathc; i++) {
		if (globbuf.gl_pathv[i][strlen(globbuf.gl_pathv[i])-1] == '/') {
			/* This is a directory - so skip */
			continue;
		}

		if (access(globbuf.gl_pathv[i], R_OK)) {
			log_message(LOG_INFO, "Unable to read configuration file %s", globbuf.gl_pathv[i]);
			ret = false;
			break;
		}

		/* Make sure that the file is a regular file, and not for example a directory or executable */
		if (stat(globbuf.gl_pathv[i], &stb) ||
		    !S_ISREG(stb.st_mode) ||
		     (stb.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
			log_message(LOG_INFO, "Configuration file '%s' is not a regular non-executable file", globbuf.gl_pathv[i]);
			ret = false;
			break;
		}

		num_matches++;
	}

	if (ret) {
		if (num_matches > 1)
			report_config_error(CONFIG_MULTIPLE_FILES, "WARNING, more than one file matches configuration file %s, using %s", conf_file, globbuf.gl_pathv[0]);
		else if (num_matches == 0) {
			report_config_error(CONFIG_FILE_NOT_FOUND, "Unable to find configuration file %s", conf_file);
			ret = false;
		}
	}

	globfree(&globbuf);

	return ret;
}

static bool
check_include(char *buf)
{
	vector_t *strvec;
	bool ret = false;
	FILE *prev_stream;

	/* Simple check first for include */
	if (!strstr(buf, "include"))
		return false;

	strvec = alloc_strvec(buf);

	if (!strvec)
		return false;

	if(!strcmp("include", vector_slot(strvec, 0)) && vector_size(strvec) == 2) {
		prev_stream = current_stream;

		read_conf_file(vector_slot(strvec, 1));

		current_stream = prev_stream;
		ret = true;
	}

	free_strvec(strvec);
	return ret;
}

static def_t *
find_definition(const char *name, size_t len, bool definition)
{
	element e;
	def_t *def;
	const char *p;
	bool using_braces = false;
	bool allow_multiline;

	if (LIST_ISEMPTY(defs))
		return NULL;

	if (!definition && *name == BOB[0]) {
		using_braces = true;
		name++;
	}

	if (!isalpha(*name) && *name != '_')
		return NULL;

	if (!len) {
		for (len = 1, p = name + 1; *p != '\0' && (isalnum(*p) || *p == '_'); len++, p++);

		/* Check we have a suitable end character */
		if (using_braces && *p != EOB[0])
			return NULL;

		if (!using_braces && !definition &&
		     *p != ' ' && *p != '\t' && *p != '\0')
			return NULL;
	}

	if (definition ||
	    (!using_braces && name[len] == '\0') ||
	    (using_braces && name[len+1] == '\0'))
		allow_multiline = true;
	else
		allow_multiline = false;

	for (e = LIST_HEAD(defs); e; ELEMENT_NEXT(e)) {
		def = ELEMENT_DATA(e);
		if (def->name_len == len &&
		    (allow_multiline || !def->multiline) &&
		    !strncmp(def->name, name, len))
			return def;
	}

	return NULL;
}

static char *
replace_param(char *buf, size_t max_len, char *multiline_ptr)
{
	char *cur_pos = buf;
	size_t len_used = strlen(buf);
	def_t *def;
	char *s, *d, *e;
	ssize_t i;
	size_t extra_braces;
	size_t replacing_len;
	char *next_ptr = NULL;

	while ((cur_pos = strchr(cur_pos, '$')) && cur_pos[1] != '\0') {
		if ((def = find_definition(cur_pos + 1, 0, false))) {
			extra_braces = cur_pos[1] == BOB[0] ? 2 : 0;
			next_ptr = multiline_ptr;

			/* We are in a multiline expansion, and now have another
			 * one, so save the previous state on the multiline stack */
			if (def->multiline && multiline_ptr) {
				if (!LIST_EXISTS(multiline_stack))
					multiline_stack = alloc_list(NULL, NULL);
				list_add(multiline_stack, multiline_ptr);
			}

			if (def->fn) {
				/* This is a standard definition that uses a function for the replacement text */
				if (def->value)
					FREE(def->value);
				def->value = (*def->fn)();
				def->value_len = strlen(def->value);
			}

			/* Ensure there is enough room to replace $PARAM or ${PARAM} with value */
			if (def->multiline) {
				replacing_len = strcspn(def->value, DEF_LINE_END);
				next_ptr = def->value + replacing_len + 1;
				multiline_ptr = next_ptr;
			}
			else
				replacing_len = def->value_len;

			if (len_used + replacing_len - (def->name_len + 1 + extra_braces) >= max_len) {
				log_message(LOG_INFO, "Parameter substitution on line '%s' would exceed maximum line length", buf);
				return NULL;
			}

			if (def->name_len + 1 + extra_braces != replacing_len) {
				/* We need to move the existing text */
				if (def->name_len + 1 + extra_braces < replacing_len) {
					/* We are lengthening the buf text */
					s = cur_pos + strlen(cur_pos);
					d = s - (def->name_len + 1 + extra_braces) + replacing_len;
					e = cur_pos;
					i = -1;
				} else {
					/* We are shortening the buf text */
					s = cur_pos + (def->name_len + 1 + extra_braces) - replacing_len;
					d = cur_pos;
					e = cur_pos + strlen(cur_pos);
					i = 1;
				}

				do {
					*d = *s;
					if (s == e)
						break;
					d += i;
					s += i;
				} while (true);

				len_used = len_used + replacing_len - (def->name_len + 1 + extra_braces);
			}

			/* Now copy the replacement text */
			strncpy(cur_pos, def->value, replacing_len);
		}
		else
			cur_pos++;
	}

	return next_ptr;
}

static void
free_definition(void *d)
{
	def_t *def = d;

	FREE(def->name);
	FREE_PTR(def->value);
	FREE(def);
}

/* A definition is of the form $NAME=TEXT */
static def_t*
check_definition(const char *buf)
{
	const char *p;
	def_t* def;
	size_t def_name_len;
	char *str;

	if (buf[0] != '$')
		return false;

	if (!isalpha(buf[1]) && buf[1] != '_')
		return false;

	for (p = &buf[2]; *p; p++) {
		if (*p == '=')
			break;
		if (!isalnum(*p) &&
		    !isdigit(*p) &&
		    *p != '_')
			return false;
	}

	if (*p != '=')
		return false;

	def_name_len = (size_t)(p - &buf[1]);
	if ((def = find_definition(&buf[1], def_name_len, true))) {
		FREE(def->value);
		def->fn = NULL;		/* Allow a standard definition to be overridden */
	}
	else {
		def = MALLOC(sizeof(*def));
		def->name_len = def_name_len;
		str = MALLOC(def->name_len + 1);
		strncpy(str, &buf[1], def->name_len);
		str[def->name_len] = '\0';
		def->name = str;

		if (!LIST_EXISTS(defs))
			defs = alloc_list(free_definition, NULL);
		list_add(defs, def);
	}

	p++;
	def->value_len = strlen(p);
	if (p[def->value_len - 1] == '\\') {
		/* Remove leading and trailing whitespace */
		while (isblank(*p))
			p++, def->value_len--;
		while (def->value_len >= 2) {
			if (isblank(p[def->value_len - 2]))
				def->value_len--;
		}
		if (def->value_len >= 2)
			def->value[def->value_len - 1] = DEF_LINE_END[0];
		else {
			p += def->value_len;
			def->value_len = 0;
		}
		def->multiline = true;
	} else
		def->multiline = false;
	str = MALLOC(def->value_len + 1);
	strcpy(str, p);
	def->value = str;

	return def;
}

static void
add_std_definition(const char *name, const char *value, char *(*fn)(void))
{
	def_t* def;

	def = MALLOC(sizeof(*def));
	def->name_len = strlen(name);
	def->name = MALLOC(def->name_len + 1);
	strcpy(def->name, name);
	if (value) {
		def->value_len = strlen(value);
		def->value = MALLOC(def->value_len + 1);
		strcpy(def->value, value);
	}
	def->fn = fn;

	if (!LIST_EXISTS(defs))
		defs = alloc_list(free_definition, NULL);
	list_add(defs, def);
}

static void
set_std_definitions(void)
{
	add_std_definition("_PWD", NULL, get_cwd);
	add_std_definition("_INSTANCE", NULL, get_instance);
}

static void
free_parser_data(void)
{
	if (LIST_EXISTS(defs))
		free_list(&defs);

	if (LIST_EXISTS(multiline_stack))
		free_list(&multiline_stack);
}

static bool
read_line(char *buf, size_t size)
{
	size_t len ;
	bool eof = false;
	size_t config_id_len;
	char *buf_start;
	bool rev_cmp;
	size_t ofs;
	char *text_start;
	bool recheck;
	static def_t *def = NULL;
	static char *next_ptr = NULL;
	bool multiline_param_def = false;
	char *new_str;
	char *end;
	static char *line_residue = NULL;
	size_t skip;
	char *p;

	config_id_len = config_id ? strlen(config_id) : 0;
	do {
		text_start = NULL;

		if (line_residue) {
			strcpy(buf, line_residue);
			FREE(line_residue);
			line_residue = NULL;
		}
		else if (next_ptr) {
			/* We are expanding a multiline parameter, so copy next line */
			end = strchr(next_ptr, DEF_LINE_END[0]);
			if (!end) {
				strcpy(buf, next_ptr);
				if (!LIST_ISEMPTY(multiline_stack)) {
					next_ptr = LIST_TAIL_DATA(multiline_stack);
					list_remove(multiline_stack, multiline_stack->tail);
				}
				else
					next_ptr = NULL;
			} else {
				strncpy(buf, next_ptr, (size_t)(end - next_ptr));
				buf[end - next_ptr] = '\0';
				next_ptr = end + 1;
			}
		}
		else if (!fgets(buf, (int)size, current_stream))
		{
			eof = true;
			buf[0] = '\0';
			break;
		}

		/* Remove trailing <CR>/<LF> */
		len = strlen(buf);
		while (len && (buf[len-1] == '\n' || buf[len-1] == '\r'))
			buf[--len] = '\0';

		/* Handle multi-line definitions */
		if (multiline_param_def) {
			/* Remove leading and trailing spaces and tabs */
			skip = strspn(buf, " \t");
			len -= skip;
			text_start = buf + skip;
			if (len && text_start[len-1] == '\\') {
				while (len >= 2 && isblank(text_start[len - 2]))
					len--;
				text_start[len-1] = DEF_LINE_END[0];
			} else {
				while (len >= 1 && isblank(text_start[len - 1]))
					len--;
				multiline_param_def = false;
			}

			/* Skip blank lines */
			if (!len ||
			    (len == 1 && multiline_param_def)) {
				buf[0] = '\0';
				continue;
			}

			/* Add the line to the definition */
			new_str = MALLOC(def->value_len + len + 1);
			strcpy(new_str, def->value);
			strncpy(new_str + def->value_len, text_start, len);
			new_str[def->value_len + len] = '\0';
			FREE(def->value);
			def->value = new_str;
			def->value_len += len;

			buf[0] = '\0';
			continue;
		}

		if (len == 0)
			continue;

		text_start = buf + strspn(buf, " \t");
		if (text_start[0] == '\0') {
			buf[0] = '\0';
			continue;
		}

		recheck = false;
		do {
			if (text_start[0] == '@') {
				/* If the line starts '@', check the following word matches the system id.
				   @^ reverses the sense of the match */
				if (text_start[1] == '^') {
					rev_cmp = true;
					ofs = 2;
				} else {
					rev_cmp = false;
					ofs = 1;
				}

				/* We need something after the system_id */
				if (!(buf_start = strpbrk(text_start + ofs, " \t"))) {
					buf[0] = '\0';
					break;
				}

				/* Check if config_id matches/doesn't match as appropriate */
				if ((!config_id ||
				     (size_t)(buf_start - (text_start + ofs)) != config_id_len ||
				     strncmp(text_start + ofs, config_id, config_id_len)) != rev_cmp) {
					buf[0] = '\0';
					break;
				}

				/* Remove the @config_id from start of line */
				memset(text_start, ' ', (size_t)(buf_start - text_start));

				text_start += strspn(text_start, " \t");
			}

			if (text_start[0] == '$' && (def = check_definition(text_start))) {
				/* check_definition() saves the definition */
				if (def->multiline)
					multiline_param_def = true;
				buf[0] = '\0';
				break;
			}

			if (!LIST_ISEMPTY(defs) && strchr(text_start, '$')) {
				next_ptr = replace_param(buf, size, next_ptr);
				text_start += strspn(text_start, " \t");
				if (text_start[0] == '@')
					recheck = true;
			}
		} while (recheck);
	} while (buf[0] == '\0' || check_include(buf));

	/* Search for BOB[0] or EOB[0] not in "" and before ! or # */
	if (buf[0] && text_start) {
		p = text_start;
		if (p[0] != BOB[0] && p[0] != EOB[0]) {
			while ((p = strpbrk(p, BOB EOB "!#\""))) {
				if (*p != '"')
					break;

				/* Skip over anything in ""s */
				if (!(p = strchr(p + 1, '"')))
					break;

				p++;
			}
		}

		if (p && (p[0] == BOB[0] || p[0] == EOB[0])) {
			if (p == text_start)
				skip = strspn(p + 1, " \t") + 1;
			else
				skip = 0;

			if (p[skip] && p[skip] != '#' && p[skip] != '!') {
				line_residue = MALLOC(strlen(p + skip) + 1);
				strcpy(line_residue, p + skip);
				p[skip] = '\0';
			}
		}
	}

	return !eof;
}

void
alloc_value_block(void (*alloc_func) (vector_t *), const char *block_type)
{
	char *buf;
	char *str = NULL;
	vector_t *vec = NULL;
	bool first_line = true;

	buf = (char *) MALLOC(MAXBUF);
	while (read_line(buf, MAXBUF)) {
		if (!(vec = alloc_strvec(buf)))
			continue;

		if (first_line) {
			first_line = false;

			if (!strcmp(vector_slot(vec, 0), BOB)) {
				free_strvec(vec);
				continue;
			}

			log_message(LOG_INFO, "'%s' missing from beginning of block %s", BOB, block_type);
		}

		str = vector_slot(vec, 0);
		if (!strcmp(str, EOB)) {
			free_strvec(vec);
			break;
		}

		if (vector_size(vec))
			(*alloc_func) (vec);

		free_strvec(vec);
	}
	FREE(buf);
}

static vector_t *read_value_block_vec;
static void
read_value_block_line(vector_t *strvec)
{
	size_t word;
	char *str;
	char *dup;

	if (!read_value_block_vec)
		read_value_block_vec = vector_alloc();

	vector_foreach_slot(strvec, str, word) {
		dup = (char *) MALLOC(strlen(str) + 1);
		strcpy(dup, str);
		vector_alloc_slot(read_value_block_vec);
		vector_set_slot(read_value_block_vec, dup);
	}
}

vector_t *
read_value_block(vector_t *strvec)
{
	vector_t *ret_vec;

	alloc_value_block(read_value_block_line, vector_slot(strvec,0));

	ret_vec = read_value_block_vec;
	read_value_block_vec = NULL;

	return ret_vec;
}

void *
set_value(vector_t *strvec)
{
	char *str;
	size_t size;
	char *alloc;

	if (vector_size(strvec) < 2)
		return NULL;

	str = vector_slot(strvec, 1);
	size = strlen(str);

	alloc = (char *) MALLOC(size + 1);
	if (!alloc)
		return NULL;

	memcpy(alloc, str, size);

	return alloc;
}

bool
read_timer(vector_t *strvec, size_t index, unsigned long *res, unsigned long min_time, unsigned long max_time, __attribute__((unused)) bool ignore_error)
{
	unsigned long timer;
	char *endptr;

	if (!max_time)
		max_time = TIMER_MAX;

	errno = 0;
	timer = strtoul(vector_slot(strvec, index), &endptr, 10);
	*res = (timer > TIMER_MAX ? TIMER_MAX : timer) * TIMER_HZ;

	if (FMT_STR_VSLOT(strvec, index)[0] == '-')
		report_config_error(CONFIG_INVALID_NUMBER, "Line starting '%s' has negative number '%s'", FMT_STR_VSLOT(strvec, 0), FMT_STR_VSLOT(strvec, index));
	else if (*endptr)
		report_config_error(CONFIG_INVALID_NUMBER, "Line starting '%s' has invalid number '%s'", FMT_STR_VSLOT(strvec, 0), FMT_STR_VSLOT(strvec, index));
	else if (errno == ERANGE || timer > TIMER_MAX)
		report_config_error(CONFIG_INVALID_NUMBER, "Line starting '%s' has number '%s' outside timer range", FMT_STR_VSLOT(strvec, 0), FMT_STR_VSLOT(strvec, index));
	else if (timer < min_time || timer > max_time)
		report_config_error(CONFIG_INVALID_NUMBER, "Line starting '%s' has number '%s' outside range [%ld, %ld]", FMT_STR_VSLOT(strvec, 0), FMT_STR_VSLOT(strvec, index), min_time, max_time ? max_time : TIMER_MAX);
	else
		return true;

#ifdef _STRICT_CONFIG_
	return false;
#else
	return ignore_error && timer >= min_time && timer <= max_time && !__test_bit(CONFIG_TEST_BIT, &debug);
#endif
}

/* Checks for on/true/yes or off/false/no */
int
check_true_false(char *str)
{
	if (!strcmp(str, "true") || !strcmp(str, "on") || !strcmp(str, "yes"))
		return true;
	if (!strcmp(str, "false") || !strcmp(str, "off") || !strcmp(str, "no"))
		return false;

	return -1;	/* error */
}

void skip_block(bool need_block_start)
{
	/* Don't process the rest of the configuration block */
	if (need_block_start)
		skip_sublevel = -1;
	else
		skip_sublevel = 1;
}

/* Data initialization */
void
init_data(const char *conf_file, vector_t * (*init_keywords) (void))
{
	/* Init Keywords structure */
	keywords = vector_alloc();

	(*init_keywords) ();

	/* Add out standard definitions */
	set_std_definitions();

#if DUMP_KEYWORDS
	/* Dump configuration */
	dump_keywords(keywords, 0, NULL);
#endif

	/* Stream handling */
	current_keywords = keywords;

	register_null_strvec_handler(null_strvec);
	read_conf_file(conf_file);
	unregister_null_strvec_handler();

	/* Close the password database if it was opened */
	endpwent();

	free_keywords(keywords);
	free_parser_data();
#ifdef _WITH_VRRP_
	clear_rt_names();
#endif
	notify_resource_release();
}
