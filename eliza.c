/*
  eliza -- a famous program from the early days of AI, written in pure C

  Copyright (C) 2026 Alexios Zavras
  SPDX-License-Identifier: GPL-3.0-or-later

  Based on the public domain C++ code by Anthony Hay (2021)
 */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPECIAL_RULE_NONE      "zNONE"
#define SPECIAL_RULE_MEMORY    "zMEMORY"
#define MEMORY_TRANSFORMATIONS 4

typedef struct {
    /* Owns every string in items. */
    char **items;
    size_t count;
    size_t capacity;
} STRING_LIST;

typedef struct {
    char  *data;
    size_t length;
    size_t capacity;
} STRING_BUILDER;

typedef struct {
    STRING_LIST  decomposition;
    STRING_LIST *reassembly;
    size_t       reassembly_count;
    size_t       reassembly_capacity;
    size_t       next_reassembly;
} TRANSFORMATION;

typedef enum {
    RULE_SUBSTITUTION,
    RULE_DLIST,
    RULE_EQUIVALENCE,
    RULE_PRE,
    RULE_MEMORY,
    RULE_VANILLA
} RULE_TYPE;

typedef struct {
    RULE_TYPE       type;
    char           *lookup_key;
    char           *keyword;
    char           *substitution;
    char           *link_keyword;
    int             precedence;
    STRING_LIST     tags;
    TRANSFORMATION *transformations;
    size_t          transformation_count;
    size_t          transformation_capacity;
    STRING_LIST     memories;
    size_t          memory_rule_index;
    unsigned        recollection_count;
} RULE;

typedef struct {
    char       *name;
    STRING_LIST words;
} TAG;

typedef struct {
    STRING_LIST hello_message;
    RULE       *rules;
    size_t      rule_count;
    size_t      rule_capacity;
} SCRIPT;

typedef struct {
    TAG   *items;
    size_t count;
    size_t capacity;
} TAG_MAP;

typedef enum {
    TOKEN_EOF,
    TOKEN_SYMBOL,
    TOKEN_NUMBER,
    TOKEN_OPEN,
    TOKEN_CLOSE
} TOKEN_TYPE;

typedef struct {
    TOKEN_TYPE  type;
    const char *value;
} TOKEN;

typedef struct {
    FILE          *file;
    int            has_lookahead;
    int            lookahead;
    int            has_token_lookahead;
    TOKEN          token_lookahead;
    STRING_BUILDER token_text;
    size_t         line;
    size_t         column;
    size_t         error_line;
    size_t         error_column;
    char           error[256];
} PARSER;

typedef enum {
    ACTION_INAPPLICABLE,
    ACTION_COMPLETE,
    ACTION_NEWKEY,
    ACTION_LINKKEY
} ACTION;

static
void
fail(const char *message)
{
    fprintf(stderr, "eliza: %s\n", message);
    exit(EXIT_FAILURE);
}

static
void *
checked_realloc(void *ptr, size_t size)
{
    if (size == 0) size = 1;
    void *result = realloc(ptr, size);
    if (result == NULL) fail("out of memory");
    return result;
}

static
void *
checked_array_realloc(void *ptr, size_t count, size_t item_size)
{
    if (item_size != 0 && count > (size_t) -1 / item_size)
	fail("allocation size overflow");
    return checked_realloc(ptr, count * item_size);
}

static
size_t
grow_capacity(size_t capacity)
{
    if (capacity == 0) return 8;
    if (capacity > (size_t) -1 / 2) fail("allocation size overflow");
    return capacity * 2;
}

static char *copy_string(const char *s);

static
void
builder_append_char(STRING_BUILDER *builder, char ch)
{
    if (builder->length > (size_t) -1 - 2) fail("string size overflow");
    if (builder->length + 1 >= builder->capacity) {
	size_t new_capacity = grow_capacity(builder->capacity);
	if (new_capacity <= builder->length + 1)
	    new_capacity = builder->length + 2;
	builder->data     = checked_realloc(builder->data, new_capacity);
	builder->capacity = new_capacity;
    }
    builder->data[builder->length++] = ch;
    builder->data[builder->length]   = '\0';
}

static
void
builder_append_text(STRING_BUILDER *builder, const char *text)
{
    while (*text != '\0') builder_append_char(builder, *text++);
}

static
char *
builder_take(STRING_BUILDER *builder)
{
    char *result;
    if (builder->data == NULL) {
	memset(builder, 0, sizeof(*builder));
	return copy_string("");
    }
    result = builder->data;
    memset(builder, 0, sizeof(*builder));
    return result;
}

static
char *
copy_string(const char *s)
{
    size_t size;
    if (s == NULL) fail("attempted to copy a null string");
    size         = strlen(s) + 1;
    char *result = malloc(size);
    if (result == NULL) fail("out of memory");
    memcpy(result, s, size);
    return result;
}

static
void
append_text(char **target, const char *text)
{
    size_t old_length  = *target == NULL ? 0 : strlen(*target);
    size_t text_length = strlen(text);
    if (text_length > (size_t) -1 - old_length - 1)
	fail("string size overflow");
    *target = checked_realloc(*target, old_length + text_length + 1);
    memcpy(*target + old_length, text, text_length + 1);
}

static
void
list_push(STRING_LIST *list, const char *value)
{
    char *copy = copy_string(value);
    if (list->count == list->capacity) {
	list->capacity = grow_capacity(list->capacity);
	list->items    = checked_array_realloc(list->items, list->capacity,
	                                       sizeof(*list->items));
    }
    list->items[list->count++] = copy;
}

/* The list takes ownership of value. */
static
void
list_push_owned(STRING_LIST *list, char *value)
{
    if (list->count == list->capacity) {
	list->capacity = grow_capacity(list->capacity);
	list->items    = checked_array_realloc(list->items, list->capacity,
	                                       sizeof(*list->items));
    }
    list->items[list->count++] = value;
}

static
void
list_push_front(STRING_LIST *list, const char *value)
{
    if (list->count == list->capacity) {
	list->capacity = grow_capacity(list->capacity);
	list->items    = checked_array_realloc(list->items, list->capacity,
	                                       sizeof(*list->items));
    }
    memmove(list->items + 1, list->items, list->count * sizeof(*list->items));
    list->items[0] = copy_string(value);
    ++list->count;
}

static
void
list_clear(STRING_LIST *list)
{
    size_t i;
    for (i = 0; i < list->count; ++i) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static
STRING_LIST
list_copy(const STRING_LIST *source)
{
    STRING_LIST result = {0};
    size_t      i;
    for (i = 0; i < source->count; ++i) list_push(&result, source->items[i]);
    return result;
}

static
void
list_append_list(STRING_LIST *target, const STRING_LIST *source)
{
    size_t i;
    for (i = 0; i < source->count; ++i) list_push(target, source->items[i]);
}

static
char *
list_join(const STRING_LIST *list)
{
    STRING_BUILDER builder = {0};
    size_t         i;
    for (i = 0; i < list->count; ++i) {
	if (list->items[i][0] == '\0') continue;
	if (builder.length != 0) builder_append_char(&builder, ' ');
	builder_append_text(&builder, list->items[i]);
    }
    return builder_take(&builder);
}

static
int
is_punctuation_char(char c)
{
    return c == ',' || c == '.';
}

static
int
is_punctuation(const char *s)
{
    return strlen(s) == 1 && is_punctuation_char(s[0]);
}

static
STRING_LIST
split_words(const char *text)
{
    STRING_LIST          result = {0};
    STRING_BUILDER       word   = {0};
    const unsigned char *p      = (const unsigned char *) text;

    while (*p != '\0') {
	if (is_punctuation_char((char) *p) || *p == ' ') {
	    if (word.length != 0) {
		char *completed = builder_take(&word);
		list_push_owned(&result, completed);
	    }
	    if (*p != ' ') {
		list_push(&result, *p == ',' ? "," : ".");
	    }
	} else {
	    builder_append_char(&word, (char) *p);
	}
	++p;
    }
    if (word.length != 0) {
	char *completed = builder_take(&word);
	list_push_owned(&result, completed);
    }
    free(word.data);
    return result;
}

static
char *
upper_copy(const char *text)
{
    char          *result = copy_string(text);
    unsigned char *p      = (unsigned char *) result;
    while (*p != '\0') {
	*p = (unsigned char) toupper(*p);
	++p;
    }
    return result;
}

static
int
to_integer(const char *text)
{
    int                  result = 0;
    const unsigned char *p      = (const unsigned char *) text;
    if (*p == '\0') return -1;
    while (*p != '\0') {
	if (!isdigit(*p)) return -1;
	if (result > (INT_MAX - (*p - '0')) / 10) return -1;
	result = result * 10 + (*p - '0');
	++p;
    }
    return result;
}

static
RULE *
find_rule(SCRIPT *script, const char *keyword)
{
    size_t i;
    for (i = 0; i < script->rule_count; ++i)
	if (strcmp(script->rules[i].lookup_key, keyword) == 0)
	    return &script->rules[i];
    return NULL;
}

static
RULE *
add_rule(SCRIPT     *script,
         RULE_TYPE   type,
         const char *keyword,
         const char *substitution,
         int         precedence)
{
    RULE *rule;
    if (script->rule_count == script->rule_capacity) {
	script->rule_capacity = script->rule_capacity == 0
	                          ? 32
	                          : grow_capacity(script->rule_capacity);
	script->rules =
	    checked_array_realloc(script->rules, script->rule_capacity,
	                          sizeof(*script->rules));
    }
    rule = &script->rules[script->rule_count++];
    memset(rule, 0, sizeof(*rule));
    rule->type              = type;
    rule->lookup_key        = copy_string(keyword);
    rule->keyword           = copy_string(keyword);
    rule->substitution      = copy_string(substitution);
    rule->precedence        = precedence;
    rule->memory_rule_index = MEMORY_TRANSFORMATIONS - 1;
    return rule;
}

static
TRANSFORMATION *
add_transformation(RULE *rule, const STRING_LIST *decomposition)
{
    TRANSFORMATION *transformation;
    if (rule->transformation_count == rule->transformation_capacity) {
	rule->transformation_capacity =
	    rule->transformation_capacity == 0
		? 4
		: grow_capacity(rule->transformation_capacity);
	rule->transformations =
	    checked_array_realloc(rule->transformations,
	                          rule->transformation_capacity,
	                          sizeof(*rule->transformations));
    }
    transformation = &rule->transformations[rule->transformation_count++];
    memset(transformation, 0, sizeof(*transformation));
    transformation->decomposition = list_copy(decomposition);
    return transformation;
}

static
void
transformation_add_reassembly(TRANSFORMATION    *transformation,
                              const STRING_LIST *reassembly)
{
    if (transformation->reassembly_count
        == transformation->reassembly_capacity) {
	transformation->reassembly_capacity =
	    transformation->reassembly_capacity == 0
		? 4
		: grow_capacity(transformation->reassembly_capacity);
	transformation->reassembly =
	    checked_array_realloc(transformation->reassembly,
	                          transformation->reassembly_capacity,
	                          sizeof(*transformation->reassembly));
    }
    transformation->reassembly[transformation->reassembly_count++] =
	list_copy(reassembly);
}

static
TAG *
find_tag(TAG_MAP *tags, const char *name)
{
    size_t i;
    for (i = 0; i < tags->count; ++i)
	if (strcmp(tags->items[i].name, name) == 0) return &tags->items[i];
    return NULL;
}

static
void
tag_add_word(TAG_MAP *tags, const char *tag_name, const char *word)
{
    TAG *tag = find_tag(tags, tag_name);
    if (tag == NULL) {
	if (tags->count == tags->capacity) {
	    tags->capacity = grow_capacity(tags->capacity);
	    tags->items    = checked_array_realloc(tags->items, tags->capacity,
	                                           sizeof(*tags->items));
	}
	tag = &tags->items[tags->count++];
	memset(tag, 0, sizeof(*tag));
	tag->name = copy_string(tag_name);
    }
    list_push(&tag->words, word);
}

static
TAG_MAP
collect_tags(const SCRIPT *script)
{
    TAG_MAP tags = {0};
    size_t  i, j;
    for (i = 0; i < script->rule_count; ++i) {
	const RULE *rule = &script->rules[i];
	for (j = 0; j < rule->tags.count; ++j) {
	    const char *tag = rule->tags.items[j];
	    if (strcmp(tag, "/") == 0) continue;
	    if (tag[0] == '/') ++tag;
	    tag_add_word(&tags, tag, rule->keyword);
	}
    }
    return tags;
}

static
int
list_contains(const STRING_LIST *list, const char *value)
{
    size_t i;
    for (i = 0; i < list->count; ++i)
	if (strcmp(list->items[i], value) == 0) return 1;
    return 0;
}

static
int
inlist(const char *word, const char *wordlist, const TAG_MAP *tags)
{
    char       *content = copy_string(wordlist);
    size_t      length  = strlen(content);
    STRING_LIST choices = {0};
    int         found   = 0;
    if (length != 0 && content[length - 1] == ')') content[length - 1] = '\0';
    if (content[0] == '(') memmove(content, content + 1, strlen(content));
    if (content[0] == '*' || content[0] == '/') {
	char *body = content + 1;
	while (*body == ' ') ++body;
	if (content[0] == '*') {
	    choices = split_words(body);
	} else {
	    size_t i;
	    for (i = 0; i < tags->count; ++i)
		if (strcmp(tags->items[i].name, body) == 0) {
		    choices = list_copy(&tags->items[i].words);
		    break;
		}
	}
    }
    found = list_contains(&choices, word);
    list_clear(&choices);
    free(content);
    return found;
}

static
int
match_pattern(const TAG_MAP     *tags,
              const STRING_LIST *pattern,
              const STRING_LIST *words,
              STRING_LIST       *components)
{
    size_t         rows, columns, cells, i, j;
    unsigned char *matches;
    int            result;

    list_clear(components);
    if (pattern->count == (size_t) -1 || words->count == (size_t) -1)
	fail("match table size overflow");
    rows    = pattern->count + 1;
    columns = words->count + 1;
    if (columns != 0 && rows > (size_t) -1 / columns)
	fail("match table size overflow");
    cells   = rows * columns;
    matches = calloc(cells, sizeof(*matches));
    if (matches == NULL) fail("out of memory");

    matches[pattern->count * columns + words->count] = 1;
    for (i = pattern->count; i-- > 0;) {
	int count = to_integer(pattern->items[i]);
	for (j = words->count + 1; j-- > 0;) {
	    size_t index = i * columns + j;
	    if (count == 0) {
		matches[index] = matches[(i + 1) * columns + j]
		              || (j < words->count && matches[index + 1]);
	    } else if (count > 0) {
		matches[index] =
		    (size_t) count <= words->count - j
		    && matches[(i + 1) * columns + j + (size_t) count];
	    } else if (j < words->count) {
		const char *pattern_word = pattern->items[i];
		int         word_matches =
		    pattern_word[0] == '('
			? inlist(words->items[j], pattern_word, tags)
			: strcmp(pattern_word, words->items[j]) == 0;
		matches[index] =
		    word_matches && matches[(i + 1) * columns + j + 1];
	    }
	}
    }

    result = matches[0] != 0;
    if (result) {
	i = 0;
	j = 0;
	while (i < pattern->count) {
	    int    count = to_integer(pattern->items[i]);
	    size_t consumed;
	    if (count == 0) {
		size_t end = j;
		while (end <= words->count && !matches[(i + 1) * columns + end])
		    ++end;
		if (end > words->count)
		    fail("inconsistent pattern match table");
		consumed = end - j;
	    } else if (count > 0) {
		consumed = (size_t) count;
	    } else {
		consumed = 1;
	    }
	    {
		STRING_BUILDER component = {0};
		size_t         k;
		for (k = 0; k < consumed; ++k) {
		    if (k != 0) builder_append_char(&component, ' ');
		    builder_append_text(&component, words->items[j + k]);
		}
		{
		    char *value = builder_take(&component);
		    list_push_owned(components, value);
		}
	    }
	    j += consumed;
	    ++i;
	}
    }
    free(matches);
    return result;
}

static
STRING_LIST
reassemble(const STRING_LIST *rule, const STRING_LIST *components)
{
    STRING_LIST result = {0};
    size_t      i;
    for (i = 0; i < rule->count; ++i) {
	int n = to_integer(rule->items[i]);
	if (n < 0) {
	    list_push(&result, rule->items[i]);
	} else if (n == 0 || (size_t) n > components->count) {
	    list_push(&result,
	              "SCRIPT-ERROR-REASSEMBLY-RULE-INDEX-OUT-OF-RANGE");
	} else {
	    STRING_LIST expanded = split_words(components->items[n - 1]);
	    list_append_list(&result, &expanded);
	    list_clear(&expanded);
	}
    }
    return result;
}

static
int
token_is_number(const char *value)
{
    const unsigned char *p = (const unsigned char *) value;
    if (*p == '\0') return 0;
    while (*p != '\0') {
	if (!isdigit(*p)) return 0;
	++p;
    }
    return 1;
}

static
void
parser_set_error(PARSER *parser, const char *message)
{
    if (parser->error[0] != '\0') return;
    snprintf(parser->error, sizeof(parser->error), "%s", message);
    parser->error_line   = parser->line;
    parser->error_column = parser->column;
}

static
int
parser_get_char(PARSER *parser)
{
    int ch;
    if (parser->has_lookahead) {
	parser->has_lookahead = 0;
	ch                    = parser->lookahead;
    } else {
	ch = fgetc(parser->file);
    }
    if (ch != EOF) {
	if (ch == '\n') {
	    ++parser->line;
	    parser->column = 1;
	} else {
	    ++parser->column;
	}
    }
    return ch;
}

static
int
parser_peek_char(PARSER *parser)
{
    if (!parser->has_lookahead) {
	parser->lookahead     = fgetc(parser->file);
	parser->has_lookahead = 1;
    }
    return parser->lookahead;
}

static
TOKEN
read_token(PARSER *parser)
{
    TOKEN           token;
    int             ch;
    STRING_BUILDER *text = &parser->token_text;
    text->length         = 0;
    if (text->data != NULL) text->data[0] = '\0';
    memset(&token, 0, sizeof(token));
    for (;;) {
	do {
	    ch = parser_get_char(parser);
	    if (ch == EOF) {
		if (ferror(parser->file))
		    parser_set_error(parser, "error reading script file");
		token.type  = TOKEN_EOF;
		token.value = "";
		return token;
	    }
	} while (ch <= 0x20 || ch == 0x7f);
	if (ch != ';') break;
	do {
	    ch = parser_get_char(parser);
	} while (
	    ch != EOF && ch != '\n' && ch != '\v' && ch != '\f' && ch != '\r'
	);
	if (ch == EOF && ferror(parser->file)) {
	    parser_set_error(parser, "error reading script file");
	    token.type  = TOKEN_EOF;
	    token.value = "";
	    return token;
	}
    }
    if (ch == '(') {
	token.type  = TOKEN_OPEN;
	token.value = "";
	return token;
    }
    if (ch == ')') {
	token.type  = TOKEN_CLOSE;
	token.value = "";
	return token;
    }

    builder_append_char(text, (char) ch);
    if (isdigit((unsigned char) ch)) {
	while (isdigit((unsigned char) parser_peek_char(parser))) {
	    builder_append_char(text, (char) parser_get_char(parser));
	}
    } else {
	for (;;) {
	    ch = parser_peek_char(parser);
	    if (ch == EOF
	        || ch == '('
	        || ch == ')'
	        || ch == ';'
	        || ch <= 0x20
	        || ch == 0x7f)
		break;
	    builder_append_char(text, (char) parser_get_char(parser));
	}
    }
    token.value = text->data;
    token.type  = token_is_number(token.value) ? TOKEN_NUMBER : TOKEN_SYMBOL;
    return token;
}

static
TOKEN
peek_token(PARSER *parser)
{
    if (!parser->has_token_lookahead) {
	parser->token_lookahead     = read_token(parser);
	parser->has_token_lookahead = 1;
    }
    return parser->token_lookahead;
}

static
TOKEN
next_token(PARSER *parser)
{
    if (parser->has_token_lookahead) {
	parser->has_token_lookahead = 0;
	return parser->token_lookahead;
    }
    return read_token(parser);
}

static
int
expect_token(PARSER *parser, TOKEN_TYPE type, TOKEN *result)
{
    *result = next_token(parser);
    return result->type == type;
}

static
int
read_list(PARSER *parser, STRING_LIST *result, int prior)
{
    TOKEN token;
    memset(result, 0, sizeof(*result));
    if (prior) {
	if (!expect_token(parser, TOKEN_OPEN, &token)) {
	    parser_set_error(parser, "expected '('");
	    return 0;
	}
    }
    for (;;) {
	token = next_token(parser);
	if (token.type == TOKEN_CLOSE) return 1;
	if (token.type == TOKEN_EOF) {
	    parser_set_error(parser, "unexpected end of file in list");
	    list_clear(result);
	    return 0;
	}
	if (token.type == TOKEN_SYMBOL || token.type == TOKEN_NUMBER) {
	    if (token.type == TOKEN_NUMBER && to_integer(token.value) < 0) {
		parser_set_error(parser, "numeric token is out of range");
		list_clear(result);
		return 0;
	    }
	    list_push(result, token.value);
	} else if (token.type == TOKEN_OPEN) {
	    STRING_BUILDER sublist = {0};
	    for (;;) {
		token = next_token(parser);
		if (token.type == TOKEN_CLOSE) break;
		if (token.type != TOKEN_SYMBOL && token.type != TOKEN_NUMBER) {
		    parser_set_error(parser, "expected a word in sublist");
		    free(sublist.data);
		    list_clear(result);
		    return 0;
		}
		if (sublist.length != 0) builder_append_char(&sublist, ' ');
		builder_append_text(&sublist, token.value);
	    }
	    {
		char *wrapped = copy_string("(");
		append_text(&wrapped, sublist.data == NULL ? "" : sublist.data);
		append_text(&wrapped, ")");
		list_push(result, wrapped);
		free(wrapped);
	    }
	    free(sublist.data);
	} else {
	    parser_set_error(parser, "expected ')' or a word");
	    list_clear(result);
	    return 0;
	}
    }
}

static
int
token_equals(const TOKEN *token, const char *value)
{
    return token->type == TOKEN_SYMBOL && strcmp(token->value, value) == 0;
}

static
int
read_pre_rule(PARSER            *parser,
              SCRIPT            *script,
              const char        *keyword,
              const char        *substitution,
              int                precedence,
              const STRING_LIST *decomposition)
{
    TOKEN           token;
    STRING_LIST     reassembly = {0};
    char           *link       = NULL;
    RULE           *rule;
    TRANSFORMATION *transformation;

    token = next_token(parser); /* PRE */
    if (!token_equals(&token, "PRE") || !read_list(parser, &reassembly, 1))
	goto failed;
    if (!expect_token(parser, TOKEN_OPEN, &token)) {
	parser_set_error(parser, "expected '(' before PRE link");
	goto failed;
    }
    token = next_token(parser);
    if (token_equals(&token, "=")) {
	token = next_token(parser);
	if (token.type != TOKEN_SYMBOL) {
	    parser_set_error(parser, "expected PRE link keyword");
	    goto failed;
	}
	link = copy_string(token.value);
    } else if (token.type == TOKEN_SYMBOL
               && token.value[0] == '='
               && token.value[1] != '\0') {
	link = copy_string(token.value + 1);
    } else {
	parser_set_error(parser, "expected PRE link keyword");
	goto failed;
    }
    if (!expect_token(parser, TOKEN_CLOSE, &token)
        || !expect_token(parser, TOKEN_CLOSE, &token)
        || !expect_token(parser, TOKEN_CLOSE, &token)
        || !expect_token(parser, TOKEN_CLOSE, &token)) {
	parser_set_error(parser, "expected ')' after PRE rule");
	goto failed;
    }

    rule = add_rule(script, RULE_PRE, keyword, substitution, precedence);
    rule->link_keyword = link;
    transformation     = add_transformation(rule, decomposition);
    transformation_add_reassembly(transformation, &reassembly);
    list_clear(&reassembly);
    return 1;

  failed:
    free(link);
    list_clear(&reassembly);
    return 0;
}

static
int
read_vanilla_rule(PARSER     *parser,
                  SCRIPT     *script,
                  const char *keyword,
                  const char *substitution,
                  int         precedence)
{
    RULE *rule = NULL;
    TOKEN token;
    for (;;) {
	STRING_LIST     decomposition = {0};
	TRANSFORMATION *transformation;
	if (!read_list(parser, &decomposition, 1)) return 0;
	token = next_token(parser);
	if (token.type != TOKEN_OPEN) {
	    parser_set_error(parser, "expected reassembly rules");
	    list_clear(&decomposition);
	    return 0;
	}
	token = peek_token(parser);
	if (token_equals(&token, "PRE")) {
	    int ok = read_pre_rule(parser, script, keyword, substitution,
	                           precedence, &decomposition);
	    list_clear(&decomposition);
	    return ok;
	}

	if (rule == NULL)
	    rule = add_rule(script, RULE_VANILLA, keyword, substitution,
	                    precedence);
	transformation = add_transformation(rule, &decomposition);
	list_clear(&decomposition);
	for (;;) {
	    STRING_LIST reassembly = {0};
	    if (!read_list(parser, &reassembly, 0)) return 0;
	    transformation_add_reassembly(transformation, &reassembly);
	    list_clear(&reassembly);
	    token = next_token(parser);
	    if (token.type == TOKEN_CLOSE) break;
	    if (token.type != TOKEN_OPEN) return 0;
	}
	token = next_token(parser);
	if (token.type == TOKEN_CLOSE) return 1;
	if (token.type != TOKEN_OPEN) return 0;
    }
}

static
int
read_memory_rule(PARSER *parser, SCRIPT *script)
{
    TOKEN  token;
    RULE  *rule;
    size_t i;
    if (!expect_token(parser, TOKEN_SYMBOL, &token)) return 0;
    rule = add_rule(script, RULE_MEMORY, token.value, "", 0);
    free(rule->lookup_key);
    rule->lookup_key        = copy_string(SPECIAL_RULE_MEMORY);
    rule->memory_rule_index = MEMORY_TRANSFORMATIONS - 1;
    for (i = 0; i < MEMORY_TRANSFORMATIONS; ++i) {
	STRING_LIST     decomposition = {0};
	STRING_LIST     reassembly    = {0};
	TRANSFORMATION *transformation;
	if (!expect_token(parser, TOKEN_OPEN, &token)) {
	    parser_set_error(parser, "expected MEMORY decomposition");
	    goto failed_iteration;
	}
	for (;;) {
	    token = next_token(parser);
	    if (token_equals(&token, "=") || token.type == TOKEN_EOF) break;
	    if (token.type != TOKEN_SYMBOL && token.type != TOKEN_NUMBER) {
		parser_set_error(parser, "expected MEMORY pattern word");
		goto failed_iteration;
	    }
	    if (token.type == TOKEN_NUMBER && to_integer(token.value) < 0) {
		parser_set_error(parser, "numeric token is out of range");
		goto failed_iteration;
	    }
	    list_push(&decomposition, token.value);
	}
	if (!token_equals(&token, "=")) {
	    parser_set_error(parser, "expected '=' in MEMORY rule");
	    goto failed_iteration;
	}
	for (;;) {
	    token = next_token(parser);
	    if (token.type == TOKEN_CLOSE || token.type == TOKEN_EOF) break;
	    if (token.type != TOKEN_SYMBOL && token.type != TOKEN_NUMBER) {
		parser_set_error(parser, "expected MEMORY response word");
		goto failed_iteration;
	    }
	    if (token.type == TOKEN_NUMBER && to_integer(token.value) < 0) {
		parser_set_error(parser, "numeric token is out of range");
		goto failed_iteration;
	    }
	    list_push(&reassembly, token.value);
	}
	if (token.type != TOKEN_CLOSE) {
	    parser_set_error(parser, "expected ')' after MEMORY response");
	    goto failed_iteration;
	}
	transformation = add_transformation(rule, &decomposition);
	transformation_add_reassembly(transformation, &reassembly);
	list_clear(&decomposition);
	list_clear(&reassembly);
	continue;

      failed_iteration:
	list_clear(&decomposition);
	list_clear(&reassembly);
	return 0;
    }
    if (!expect_token(parser, TOKEN_CLOSE, &token)) {
	parser_set_error(parser, "expected ')' after MEMORY rule");
	return 0;
    }
    return 1;
}

static
int
read_dlist(PARSER     *parser,
           SCRIPT     *script,
           const char *keyword,
           const char *substitution)
{
    RULE *rule = add_rule(script, RULE_DLIST, keyword, substitution, 0);
    TOKEN token;
    if (!read_list(parser, &rule->tags, 1)) return 0;
    return expect_token(parser, TOKEN_CLOSE, &token);
}

static
int
read_equivalence(PARSER     *parser,
                 SCRIPT     *script,
                 const char *keyword,
                 const char *substitution,
                 int         precedence)
{
    TOKEN token;
    char *link;
    RULE *rule;
    token = next_token(parser);
    if (token_equals(&token, "=")) {
	token = next_token(parser);
	if (token.type != TOKEN_SYMBOL) return 0;
	link = copy_string(token.value);
    } else if (token.type == TOKEN_SYMBOL
               && token.value[0] == '='
               && token.value[1] != '\0') {
	link = copy_string(token.value + 1);
    } else {
	return 0;
    }
    if (!expect_token(parser, TOKEN_CLOSE, &token)
        || !expect_token(parser, TOKEN_CLOSE, &token)) {
	free(link);
	return 0;
    }
    rule =
	add_rule(script, RULE_EQUIVALENCE, keyword, substitution, precedence);
    rule->link_keyword = link;
    return 1;
}

static
int
read_rule(PARSER *parser, SCRIPT *script)
{
    TOKEN token;
    char *keyword;
    char *substitution = copy_string("");
    int   precedence   = 0;

    token = next_token(parser);
    if (token.type == TOKEN_EOF) {
	free(substitution);
	return 0;
    }
    if (token.type != TOKEN_OPEN) {
	parser_set_error(parser, "expected '(' before rule");
	free(substitution);
	return -1;
    }
    token = next_token(parser);
    if (token.type == TOKEN_CLOSE) {
	free(substitution);
	return 1;
    }
    if (token.type != TOKEN_SYMBOL) {
	parser_set_error(parser, "expected rule keyword, MEMORY, or NONE");
	free(substitution);
	return -1;
    }
    if (strcmp(token.value, "MEMORY") == 0) {
	int ok = read_memory_rule(parser, script);
	if (!ok) parser_set_error(parser, "malformed MEMORY rule");
	free(substitution);
	return ok ? 1 : -1;
    }
    if (strcmp(token.value, "NONE") == 0) {
	if (next_token(parser).type != TOKEN_OPEN) {
	    parser_set_error(parser, "expected NONE rule body");
	    free(substitution);
	    return -1;
	}
	{
	    int ok =
		read_vanilla_rule(parser, script, SPECIAL_RULE_NONE, "", 0);
	    if (!ok) parser_set_error(parser, "malformed NONE rule");
	    free(substitution);
	    return ok ? 1 : -1;
	}
    }
    keyword = copy_string(token.value);

    for (;;) {
	token = next_token(parser);
	if (token_equals(&token, "=")) {
	    token = next_token(parser);
	    if (token.type != TOKEN_SYMBOL) {
		parser_set_error(parser, "expected substitution word");
		break;
	    }
	    free(substitution);
	    substitution = copy_string(token.value);
	} else if (token.type == TOKEN_NUMBER) {
	    precedence = to_integer(token.value);
	    if (precedence < 0) {
		parser_set_error(parser, "rule precedence is out of range");
		break;
	    }
	} else if (token_equals(&token, "DLIST")) {
	    int ok = read_dlist(parser, script, keyword, substitution);
	    if (!ok) parser_set_error(parser, "malformed DLIST rule");
	    free(keyword);
	    free(substitution);
	    return ok ? 1 : -1;
	} else if (token.type == TOKEN_OPEN) {
	    token = peek_token(parser);
	    if (token.type == TOKEN_SYMBOL && token.value[0] == '=') {
		int ok = read_equivalence(parser, script, keyword, substitution,
		                          precedence);
		if (!ok) parser_set_error(parser, "malformed equivalence rule");
		free(keyword);
		free(substitution);
		return ok ? 1 : -1;
	    } else {
		int ok = read_vanilla_rule(parser, script, keyword,
		                           substitution, precedence);
		if (!ok)
		    parser_set_error(parser, "malformed transformation rule");
		free(keyword);
		free(substitution);
		return ok ? 1 : -1;
	    }
	} else if (token.type == TOKEN_CLOSE) {
	    add_rule(script, RULE_SUBSTITUTION, keyword, substitution, 0);
	    free(keyword);
	    free(substitution);
	    return 1;
	} else {
	    parser_set_error(parser, "malformed rule");
	    break;
	}
    }
    free(keyword);
    free(substitution);
    return -1;
}

static
int
read_script(FILE *file, SCRIPT *script, char *error, size_t error_size)
{
    PARSER parser;
    TOKEN  token;
    int    status;
    memset(&parser, 0, sizeof(parser));
    parser.file   = file;
    parser.line   = 1;
    parser.column = 1;
    if (!read_list(&parser, &script->hello_message, 1)) {
	parser_set_error(&parser, "expected opening message");
	goto failed;
    }
    token = peek_token(&parser);
    if (token_equals(&token, "START")) (void) next_token(&parser);
    while ((status = read_rule(&parser, script)) > 0);
    if (status < 0) {
	parser_set_error(&parser, "malformed script rule");
	goto failed;
    }
    if (parser.error[0] != '\0') goto failed;
    if (find_rule(script, SPECIAL_RULE_NONE) == NULL) {
	parser_set_error(&parser, "script is missing its NONE rule");
	goto failed;
    }
    {
	RULE *memory_rule = find_rule(script, SPECIAL_RULE_MEMORY);
	if (memory_rule == NULL
	    || memory_rule->transformation_count != MEMORY_TRANSFORMATIONS) {
	    parser_set_error(&parser, "script is missing a valid MEMORY rule");
	    goto failed;
	}
    }
    free(parser.token_text.data);
    return 1;

  failed:
    snprintf(error, error_size, "%zu:%zu: %.180s", parser.error_line,
             parser.error_column, parser.error);
    free(parser.token_text.data);
    return 0;
}

static
int
substitution_for(RULE *rule, char **word)
{
    if (rule->substitution[0] == '\0' || strcmp(rule->keyword, *word) != 0)
	return 0;
    free(*word);
    *word = copy_string(rule->substitution);
    return 1;
}

static
STRING_LIST
transform_words(const STRING_LIST *rule, const STRING_LIST *components)
{
    return reassemble(rule, components);
}

static
ACTION
apply_transform(RULE          *rule,
                STRING_LIST   *words,
                const TAG_MAP *tags,
                char         **link_keyword)
{
    size_t      i;
    STRING_LIST components = {0};
    if (rule->type == RULE_EQUIVALENCE) {
	*link_keyword = copy_string(rule->link_keyword);
	return ACTION_LINKKEY;
    }
    if (rule->type == RULE_PRE) {
	if (rule->transformation_count != 1
	    || rule->transformations[0].reassembly_count != 1)
	    return ACTION_INAPPLICABLE;
	if (!match_pattern(tags, &rule->transformations[0].decomposition, words,
	                   &components))
	    return ACTION_INAPPLICABLE;
	list_clear(words);
	*words = transform_words(&rule->transformations[0].reassembly[0],
	                         &components);
	list_clear(&components);
	*link_keyword = copy_string(rule->link_keyword);
	return ACTION_LINKKEY;
    }
    if (rule->type != RULE_VANILLA) return ACTION_INAPPLICABLE;

    for (i = 0; i < rule->transformation_count; ++i) {
	TRANSFORMATION *transformation = &rule->transformations[i];
	STRING_LIST     reassembled;
	STRING_LIST    *reassembly;
	if (transformation->reassembly_count == 0
	    || !match_pattern(tags, &transformation->decomposition, words,
	                      &components))
	    continue;
	reassembly =
	    &transformation->reassembly[transformation->next_reassembly];
	transformation->next_reassembly = (transformation->next_reassembly + 1)
	                                % transformation->reassembly_count;
	if (reassembly->count == 1
	    && strcmp(reassembly->items[0], "NEWKEY") == 0) {
	    list_clear(&components);
	    return ACTION_NEWKEY;
	}
	if (reassembly->count == 1
	    && reassembly->items[0][0] == '='
	    && reassembly->items[0][1] != '\0') {
	    *link_keyword = copy_string(reassembly->items[0] + 1);
	    list_clear(&components);
	    return ACTION_LINKKEY;
	}
	reassembled = transform_words(reassembly, &components);
	list_clear(words);
	*words = reassembled;
	list_clear(&components);
	return ACTION_COMPLETE;
    }
    list_clear(&components);
    return ACTION_INAPPLICABLE;
}

static
void
create_memory(RULE              *memory_rule,
              const char        *keyword,
              const STRING_LIST *words,
              const TAG_MAP     *tags)
{
    TRANSFORMATION *transformation;
    STRING_LIST     components = {0};
    STRING_LIST     reassembled;
    char           *memory;
    if (memory_rule == NULL
        || strcmp(memory_rule->keyword, keyword) != 0
        || memory_rule->transformation_count != MEMORY_TRANSFORMATIONS)
	return;
    transformation =
	&memory_rule->transformations[memory_rule->memory_rule_index];
    memory_rule->memory_rule_index = (size_t) (rand() % MEMORY_TRANSFORMATIONS);
    if (transformation->reassembly_count != 1
        || !match_pattern(tags, &transformation->decomposition, words,
                          &components)) {
	list_clear(&components);
	return;
    }
    reassembled = reassemble(&transformation->reassembly[0], &components);
    memory      = list_join(&reassembled);
    list_push_owned(&memory_rule->memories, memory);
    list_clear(&reassembled);
    list_clear(&components);
}

static
int
time_for_recollection(RULE *memory_rule)
{
    if (memory_rule == NULL || memory_rule->memories.count == 0) return 0;
    return memory_rule->recollection_count++ % 3 == 0;
}

static
char *
recall_memory(RULE *memory_rule)
{
    char  *result;
    size_t i;
    if (memory_rule == NULL || memory_rule->memories.count == 0)
	return copy_string("");
    result = memory_rule->memories.items[0];
    for (i = 1; i < memory_rule->memories.count; ++i)
	memory_rule->memories.items[i - 1] = memory_rule->memories.items[i];
    --memory_rule->memories.count;
    return result;
}

static
char *
eliza(SCRIPT *script, const char *input)
{
    char       *uppercase   = upper_copy(input);
    STRING_LIST words       = split_words(uppercase);
    STRING_LIST keystack    = {0};
    TAG_MAP     tags        = collect_tags(script);
    RULE       *memory_rule = find_rule(script, SPECIAL_RULE_MEMORY);
    int         top_rank    = 0;
    int         completed   = 0;
    size_t      i           = 0;
    char       *result;

    free(uppercase);
    while (i < words.count) {
	RULE *rule;
	if (is_punctuation(words.items[i])) {
	    if (keystack.count == 0) {
		size_t removed = i + 1;
		size_t j;
		for (j = 0; j < removed; ++j) free(words.items[j]);
		memmove(words.items, words.items + removed,
		        (words.count - removed) * sizeof(*words.items));
		words.count -= removed;
		i            = 0;
		continue;
	    }
	    {
		size_t punctuation_index = i;
		while (i < words.count) free(words.items[i++]);
		words.count = punctuation_index;
		i           = punctuation_index;
	    }
	    break;
	}
	rule = find_rule(script, words.items[i]);
	if (rule != NULL) {
	    if (rule->type == RULE_VANILLA
	        || rule->type == RULE_PRE
	        || rule->type == RULE_EQUIVALENCE) {
		if (rule->precedence > top_rank) {
		    list_push_front(&keystack, words.items[i]);
		    top_rank = rule->precedence;
		} else {
		    list_push(&keystack, words.items[i]);
		}
	    }
	    substitution_for(rule, &words.items[i]);
	}
	++i;
    }

    if (keystack.count == 0 && time_for_recollection(memory_rule)) {
	result = recall_memory(memory_rule);
	goto done;
    }

    while (keystack.count != 0) {
	char  *top_keyword  = keystack.items[0];
	RULE  *rule         = find_rule(script, top_keyword);
	char  *link_keyword = NULL;
	ACTION action;
	size_t j;
	for (j = 1; j < keystack.count; ++j)
	    keystack.items[j - 1] = keystack.items[j];
	--keystack.count;
	if (rule == NULL) {
	    free(top_keyword);
	    break;
	}
	create_memory(memory_rule, top_keyword, &words, &tags);
	action = apply_transform(rule, &words, &tags, &link_keyword);
	if (action == ACTION_COMPLETE) {
	    completed = 1;
	    free(link_keyword);
	    free(top_keyword);
	    break;
	}
	free(top_keyword);
	if (action == ACTION_INAPPLICABLE) break;
	if (action == ACTION_LINKKEY) {
	    list_push_front(&keystack, link_keyword);
	    free(link_keyword);
	}
    }

    if (completed) {
	result = list_join(&words);
	goto done;
    }

    {
	RULE *none_rule = find_rule(script, SPECIAL_RULE_NONE);
	char *discard   = NULL;
	if (none_rule != NULL)
	    (void) apply_transform(none_rule, &words, &tags, &discard);
	free(discard);
    }
    result = list_join(&words);

  done:
    list_clear(&words);
    list_clear(&keystack);
    for (i = 0; i < tags.count; ++i) {
	free(tags.items[i].name);
	list_clear(&tags.items[i].words);
    }
    free(tags.items);
    return result;
}

static
void
write_response(const char *text)
{
    puts(text);
}

static
char *
read_line(FILE *input)
{
    char  *line     = NULL;
    size_t length   = 0;
    size_t capacity = 0;
    int    ch;

    while ((ch = fgetc(input)) != EOF) {
	if (length == (size_t) -1) fail("input line is too long");
	if (length + 1 >= capacity) {
	    if (capacity > (size_t) -1 / 2) fail("input line is too long");
	    capacity = capacity == 0 ? 128 : capacity * 2;
	    line     = checked_realloc(line, capacity);
	}
	line[length++] = (char) ch;
	if (ch == '\n') break;
    }
    if (ch == EOF && ferror(input)) {
	free(line);
	fail("error reading input");
    }
    if (length == 0 && ch == EOF) {
	free(line);
	return NULL;
    }
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
	--length;
    line[length] = '\0';
    return line;
}

static
void
destroy_script(SCRIPT *script)
{
    size_t i, j, k;
    list_clear(&script->hello_message);
    for (i = 0; i < script->rule_count; ++i) {
	RULE *rule = &script->rules[i];
	free(rule->lookup_key);
	free(rule->keyword);
	free(rule->substitution);
	free(rule->link_keyword);
	list_clear(&rule->tags);
	list_clear(&rule->memories);
	for (j = 0; j < rule->transformation_count; ++j) {
	    TRANSFORMATION *transformation = &rule->transformations[j];
	    list_clear(&transformation->decomposition);
	    for (k = 0; k < transformation->reassembly_count; ++k)
		list_clear(&transformation->reassembly[k]);
	    free(transformation->reassembly);
	}
	free(rule->transformations);
    }
    free(script->rules);
    memset(script, 0, sizeof(*script));
}

static unsigned test_count;
static unsigned test_failures;

static
void
test_check(int condition, const char *description)
{
    ++test_count;
    if (!condition) {
	++test_failures;
	fprintf(stderr, "FAIL: %s\n", description);
    }
}

static
int
parse_test_text(const char *text,
                SCRIPT     *script,
                char       *error,
                size_t      error_size)
{
    FILE *file = tmpfile();
    int   result;
    if (file == NULL) {
	snprintf(error, error_size, "could not create test input");
	return 0;
    }
    if (fwrite(text, 1, strlen(text), file) != strlen(text)) {
	snprintf(error, error_size, "could not write test input");
	fclose(file);
	return 0;
    }
    rewind(file);
    result = read_script(file, script, error, error_size);
    fclose(file);
    return result;
}

static
void
test_string_matching(void)
{
    STRING_LIST pattern = {0};
    STRING_LIST words;
    STRING_LIST components = {0};
    TAG_MAP     tags       = {0};
    char       *long_word;
    size_t      i;

    list_push(&pattern, "0");
    list_push(&pattern, "YOU");
    list_push(&pattern, "(* WANT NEED)");
    list_push(&pattern, "0");
    words = split_words("YOU NEED NICE FOOD");
    test_check(match_pattern(&tags, &pattern, &words, &components),
               "zero-width and grouped pattern matches");
    test_check(components.count == 4
                   && strcmp(components.items[0], "") == 0
                   && strcmp(components.items[1], "YOU") == 0
                   && strcmp(components.items[2], "NEED") == 0
                   && strcmp(components.items[3], "NICE FOOD") == 0,
               "pattern components preserve first-valid wildcard matching");

    list_clear(&pattern);
    list_push(&pattern, "2");
    list_push(&pattern, "FOOD");
    test_check(!match_pattern(&tags, &pattern, &words, &components),
               "fixed-length mismatch is rejected");
    test_check(components.count == 0,
               "failed match leaves no stale components");

    list_clear(&pattern);
    list_clear(&words);
    for (i = 0; i < 24; ++i) list_push(&pattern, "0");
    list_push(&pattern, "IMPOSSIBLE");
    {
	STRING_BUILDER many_words = {0};
	for (i = 0; i < 150; ++i) {
	    if (i != 0) builder_append_char(&many_words, ' ');
	    builder_append_text(&many_words, "WORD");
	}
	{
	    char *text = builder_take(&many_words);
	    words      = split_words(text);
	    free(text);
	}
    }
    test_check(!match_pattern(&tags, &pattern, &words, &components),
               "many wildcards fail in bounded dynamic-programming time");

    long_word = malloc(8193);
    if (long_word == NULL) fail("out of memory in test");
    memset(long_word, 'X', 8192);
    long_word[8192] = '\0';
    {
	STRING_LIST long_words = split_words(long_word);
	test_check(long_words.count == 1 && strlen(long_words.items[0]) == 8192,
	           "word splitting has no fixed word-length limit");
	list_clear(&long_words);
    }
    free(long_word);

    list_clear(&pattern);
    list_clear(&words);
    list_clear(&components);
}

static
void
test_script_and_responses(void)
{
    static const char source[] =
	"(HELLO)\n"
	"START\n"
	"(MOTHER DLIST(/FAMILY))\n"
	"(MOM = MOTHER DLIST(/FAMILY))\n"
	"(DAD = FATHER DLIST(/FAMILY))\n"
	"(DONT = DON'T)\n"
	"(MY = YOUR 2\n"
	"    ((0 YOUR 0 (/FAMILY) 0)\n"
	"        (FAMILY)))\n"
	"(I'M = YOU'RE\n"
	"    ((0 YOU'RE 0)\n"
	"        (PRE (YOU ARE 3) (=I))))\n"
	"(I\n"
	"    ((0 YOU ARE 0)\n"
	"        (YOU SAID 4)))\n"
	"(HELLO\n"
	"    ((0)\n"
	"        (HI)))\n"
	"(HOW 10 (= WHAT))\n"
	"(WHAT\n"
	"    ((0)\n"
	"        (LINKED)))\n"
	"(MEMORY MY\n"
	"    (0 YOUR 0 = FIRST 3)\n"
	"    (0 YOUR 0 = SECOND 3)\n"
	"    (0 YOUR 0 = THIRD 3)\n"
	"    (0 YOUR 0 = RECALL 3))\n"
	"(NONE\n"
	"    ((0)\n"
	"        (FALLBACK 1)))\n"
	"()\n";
    SCRIPT script = {0};
    char   error[256];
    char  *response;

    {
	int parsed = parse_test_text(source, &script, error, sizeof(error));
	test_check(parsed, "complete script parses");
	if (!parsed) {
	    fprintf(stderr, "parser test detail: %s\n", error);
	    destroy_script(&script);
	    return;
	}
    }
    if (script.rule_count == 0) {
	destroy_script(&script);
	return;
    }

    response = eliza(&script, "dont");
    test_check(strcmp(response, "FALLBACK DON'T") == 0,
               "word substitution is applied before fallback");
    free(response);

    response = eliza(&script, "my mom");
    test_check(strcmp(response, "FAMILY") == 0,
               "DLIST tags and MY transformation work");
    free(response);

    response = eliza(&script, "unrecognized");
    test_check(strcmp(response, "RECALL MOTHER") == 0,
               "memory is recalled on a keyword-free turn");
    free(response);

    response = eliza(&script, "i'm tired");
    test_check(strcmp(response, "YOU SAID TIRED") == 0,
               "PRE transformation links to the target keyword");
    free(response);

    response = eliza(&script, "how");
    test_check(strcmp(response, "LINKED") == 0,
               "equivalence rule links to the target keyword");
    free(response);

    response = eliza(&script, "hello, unrecognized");
    test_check(strcmp(response, "HI") == 0,
               "punctuation truncates after the first clause with a keyword");
    free(response);

    response = eliza(&script, "unrecognized, hello");
    test_check(strcmp(response, "HI") == 0,
               "punctuation discards a leading clause without keywords");
    free(response);

    destroy_script(&script);
}

static
void
test_parser_errors_and_long_tokens(void)
{
    static const char invalid[] =
	"(HELLO)\n"
	"(BROKEN ((0) (incomplete";
    static const char missing_rules[] = "(HELLO)\n()";
    static const char overflowing_number[] =
	"(HELLO)\n"
	"(MEMORY KEY (0 = A) (0 = B) (0 = C) (0 = D))\n"
	"(NONE ((999999999999999999999999) (FALLBACK)))\n"
	"()\n";
    SCRIPT         script = {0};
    char           error[256];
    STRING_BUILDER source = {0};
    size_t         i;

    test_check(
	!parse_test_text(invalid, &script, error, sizeof(error))
            && strstr(error, "2:") != NULL,
	"malformed scripts report a line and column and clean partial rules"
    );
    destroy_script(&script);

    test_check(!parse_test_text(missing_rules, &script, error, sizeof(error))
                   && strstr(error, "NONE") != NULL,
               "scripts missing required special rules are rejected");
    destroy_script(&script);

    test_check(!parse_test_text(overflowing_number, &script, error,
                                sizeof(error))
                   && strstr(error, "out of range") != NULL,
               "oversized numeric tokens are rejected with a useful error");
    destroy_script(&script);

    builder_append_char(&source, '(');
    for (i = 0; i < 5000; ++i) builder_append_char(&source, 'X');
    builder_append_text(&source,
                        ")\n"
                        "(MEMORY KEY\n"
                        "    (0 A 0 = FIRST 3)\n"
                        "    (0 A 0 = SECOND 3)\n"
                        "    (0 A 0 = THIRD 3)\n"
                        "    (0 A 0 = RECALL 3))\n"
                        "(NONE ((0) (FALLBACK)))\n"
                        "()\n");
    {
	char *text = builder_take(&source);
	test_check(parse_test_text(text, &script, error, sizeof(error)),
	           "script parser accepts tokens longer than fixed buffers");
	test_check(script.hello_message.count == 1
	               && strlen(script.hello_message.items[0]) == 5000,
	           "long script token is preserved");
	free(text);
    }
    destroy_script(&script);
}

static
int
run_tests(void)
{
    test_count    = 0;
    test_failures = 0;
    test_string_matching();
    test_script_and_responses();
    test_parser_errors_and_long_tokens();
    if (test_failures != 0) {
	fprintf(stderr, "%u of %u tests failed\n", test_failures, test_count);
	return EXIT_FAILURE;
    }
    printf("All %u tests passed\n", test_count);
    return EXIT_SUCCESS;
}

int
main(int argc, char **argv)
{
    SCRIPT script = {0};
    FILE  *script_file;
    char   error[256];
    char  *greeting;

    if (argc == 2 && strcmp(argv[1], "--test") == 0) return run_tests();
    if (argc != 2) {
	fprintf(
	    stderr,
	    "Usage: %s <scriptfile>\n"
	    "  where <scriptfile> is in original 1966 ELIZA script format\n",
	    argv[0]
	);
	return EXIT_FAILURE;
    }
    script_file = fopen(argv[1], "r");
    if (script_file == NULL) {
	fprintf(stderr, "%s: failed to open script file '%s'\n", argv[0],
	        argv[1]);
	return EXIT_FAILURE;
    }
    if (!read_script(script_file, &script, error, sizeof(error))) {
	fprintf(stderr, "%s: %s\n", argv[0], error);
	fclose(script_file);
	destroy_script(&script);
	return EXIT_FAILURE;
    }
    fclose(script_file);

    greeting = list_join(&script.hello_message);
    write_response(greeting);
    free(greeting);

    for (;;) {
	char *line;
	char *response;
	fputc('\n', stdout);
	fflush(stdout);
	line = read_line(stdin);
	if (line == NULL) break;
	response = eliza(&script, line);
	write_response(response);
	free(line);
	free(response);
    }
    destroy_script(&script);
    return EXIT_SUCCESS;
}
