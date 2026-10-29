/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "hl.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

/* estados que pasan a la línea siguiente */
enum { ST_NONE, ST_CBLOCK, ST_PY3D, ST_PY3S, ST_HTMLCOM, ST_LUABLOCK };

/* C, C++, Java, JavaScript, C#, Go, Rust, Kotlin, Swift y PHP comparten casi todo: una sola lista */
static const char *const KW_C[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "goto",
    "sizeof", "typedef", "struct", "union", "enum", "static", "const", "extern", "inline", "volatile",
    "unsigned", "signed", "void", "char", "short", "int", "long", "float", "double", "bool", "boolean",
    "true", "false", "NULL", "null", "nullptr", "auto", "class", "public", "private", "protected", "virtual",
    "override", "template", "typename", "namespace", "using", "new", "delete", "this", "throw", "throws",
    "try", "catch", "finally", "final", "import", "package", "extends", "implements", "interface",
    "abstract", "var", "let", "function", "async", "await", "yield", "of", "in", "instanceof", "typeof",
    "export", "from", "func", "go", "defer", "chan", "map", "range", "type", "fn", "mut", "impl", "trait",
    "pub", "use", "mod", "match", "loop", "where", "self", "super", "crate", "val", "fun", "when",
    "string", "String", "u8", "u16", "u32", "u64", "s32", "i32", "i64", "size_t", "echo", NULL};
static const char *const KW_PY[] = {
    "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif", "else",
    "except", "False", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "None",
    "nonlocal", "not", "or", "pass", "raise", "return", "True", "try", "while", "with", "yield", "self",
    "print", "len", "range", "int", "str", "float", "list", "dict", "set", "tuple", "bool", NULL};
static const char *const KW_LUA[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
    "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while", "require", NULL};
/* shell y los scripts de GodMode9 */
static const char *const KW_SH[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "do", "done", "case", "esac", "function", "return",
    "in", "echo", "export", "local", "set", "end", "goto", "labelsel", "ask", "input", "filesel", "dirsel",
    "allow", "cp", "mv", "inject", "fill", "rm", "mkdir", "mount", "umount", "find", "findnot", "sha",
    "verify", "decrypt", "encrypt", "buildcia", "install", "extract", "chk", "reboot", "poweroff", NULL};
static const char *const KW_SQL[] = {
    "select", "from", "where", "insert", "into", "update", "delete", "create", "table", "drop", "alter",
    "join", "left", "right", "inner", "outer", "on", "group", "by", "order", "having", "limit", "values",
    "set", "and", "or", "not", "null", "as", "distinct", "primary", "key", "integer", "text", "real", NULL};
static const char *const KW_JSON[] = {"true", "false", "null", NULL};

static const struct {
	const char *ext;
	int lang;
} EXT[] = {
    {"c", HL_C},     {"h", HL_C},       {"cpp", HL_C},    {"hpp", HL_C},     {"cc", HL_C},    {"cxx", HL_C},
    {"java", HL_C},  {"js", HL_C},      {"ts", HL_C},     {"cs", HL_C},      {"go", HL_C},    {"rs", HL_C},
    {"kt", HL_C},    {"swift", HL_C},   {"php", HL_C},    {"py", HL_PY},     {"lua", HL_LUA}, {"sh", HL_SH},
    {"gm9", HL_SH},  {"yml", HL_SH},    {"yaml", HL_SH},  {"ini", HL_INI},   {"cfg", HL_INI}, {"conf", HL_INI},
    {"toml", HL_INI}, {"sql", HL_SQL},  {"html", HL_HTML}, {"htm", HL_HTML}, {"xml", HL_HTML}, {"css", HL_CSS},
    {"json", HL_JSON}, {"md", HL_MD},
};

int hl_lang(const char *ext)
{
	for (size_t i = 0; ext && i < sizeof EXT / sizeof EXT[0]; i++)
		if (!strcasecmp(ext, EXT[i].ext))
			return EXT[i].lang;
	return HL_NONE;
}

typedef struct {
	HlSpan *out;
	int max, n;
} Em;

/* añade un tramo; si es del mismo color que el anterior, lo alarga */
static void emit(Em *e, int start, int len, int c)
{
	if (len <= 0 || !e->out)
		return;
	HlSpan *last = e->n ? &e->out[e->n - 1] : NULL;
	if (last && (last->c == c || e->n == e->max) && last->start + last->len == start) {
		last->len += len; /* sin sitio: el resto va con el último color */
		return;
	}
	if (e->n < e->max)
		e->out[e->n++] = (HlSpan){start, len, (unsigned char)c};
}

static bool is_kw(int lang, const char *s, int n)
{
	const char *const *k = lang == HL_C ? KW_C : lang == HL_PY ? KW_PY : lang == HL_LUA ? KW_LUA : lang == HL_SH ? KW_SH
	                     : lang == HL_SQL ? KW_SQL : lang == HL_JSON ? KW_JSON : NULL;
	for (; k && *k; k++)
		if ((int)strlen(*k) == n && (lang == HL_SQL ? !strncasecmp(*k, s, n) : !strncmp(*k, s, n)))
			return true;
	return false;
}

static bool idch(char c)
{
	return isalnum((unsigned char)c) || c == '_';
}

static int skip_sp(const char *s, int i)
{
	while (s[i] == ' ' || s[i] == '\t')
		i++;
	return i;
}

/* hasta dónde llega un texto entre comillas que empieza en i (con escapes \) */
static int str_end(const char *s, int n, int i)
{
	char q = s[i];
	int j = i + 1;
	while (j < n && s[j] != q) {
		if (s[j] == '\\' && j + 1 < n)
			j++;
		j++;
	}
	return j < n ? j + 1 : n;
}

/* fin de un bloque: busca el cierre desde i; si está, sale del estado */
static int block(Em *e, const char *s, int n, int i, const char *close, int col, int *st)
{
	const char *q = strstr(s + i, close);
	int end = q ? (int)(q - s) + (int)strlen(close) : n;
	emit(e, i, end - i, col);
	if (q)
		*st = ST_NONE;
	return end;
}

/* una etiqueta HTML/XML: <nombre atributo="valor"> */
static int tag(Em *e, const char *s, int n, int i)
{
	int j = i + 1;
	if (s[j] == '/' || s[j] == '?' || s[j] == '!')
		j++;
	while (j < n && (idch(s[j]) || s[j] == '-' || s[j] == ':'))
		j++;
	emit(e, i, j - i, HC_TAG);
	while (j < n && s[j] != '>') {
		if (s[j] == '"' || s[j] == '\'') {
			int k = str_end(s, n, j);
			emit(e, j, k - j, HC_STR);
			j = k;
		} else if (idch(s[j])) {
			int k = j;
			while (k < n && (idch(s[k]) || s[k] == '-' || s[k] == ':'))
				k++;
			emit(e, j, k - j, HC_ATTR);
			j = k;
		} else {
			emit(e, j, 1, s[j] == '/' || s[j] == '?' ? HC_TAG : HC_TEXT);
			j++;
		}
	}
	if (j < n) {
		emit(e, j, 1, HC_TAG);
		j++;
	}
	return j;
}

int hl_line(int lang, const char *s, int *state, HlSpan *out, int max)
{
	Em e = {out, max, 0};
	int n = (int)strlen(s), i = 0, st = *state;
	int first = skip_sp(s, 0); /* primer carácter que no es espacio */
	if (lang == HL_NONE) {
		emit(&e, 0, n, HC_TEXT);
		*state = ST_NONE;
		return e.n;
	}
	/* líneas enteras: títulos de Markdown, secciones y comentarios de INI */
	if ((lang == HL_MD && s[first] == '#') || (lang == HL_INI && s[first] == '[')) {
		emit(&e, 0, n, HC_HEAD);
		return e.n;
	}
	if (lang == HL_INI && (s[first] == ';' || s[first] == '#')) {
		emit(&e, 0, n, HC_COM);
		return e.n;
	}
	if (lang == HL_INI) { /* clave = valor */
		int k = first;
		while (k < n && s[k] != '=' && s[k] != ':')
			k++;
		if (k < n) {
			emit(&e, 0, k, HC_ATTR);
			i = k;
		}
	}
	if (lang == HL_C && s[first] == '#') { /* #include, #define… */
		int k = first + 1;
		while (k < n && idch(s[k]))
			k++;
		emit(&e, 0, k, HC_KEY);
		i = k;
	}
	while (i < n) {
		if (st == ST_CBLOCK) {
			i = block(&e, s, n, i, "*/", HC_COM, &st);
			continue;
		}
		if (st == ST_HTMLCOM) {
			i = block(&e, s, n, i, "-->", HC_COM, &st);
			continue;
		}
		if (st == ST_LUABLOCK) {
			i = block(&e, s, n, i, "]]", HC_COM, &st);
			continue;
		}
		if (st == ST_PY3D || st == ST_PY3S) {
			i = block(&e, s, n, i, st == ST_PY3D ? "\"\"\"" : "'''", HC_STR, &st);
			continue;
		}
		char c = s[i], c1 = i + 1 < n ? s[i + 1] : 0;
		/* comentarios */
		if ((lang == HL_C || lang == HL_CSS || lang == HL_JSON) && c == '/' && c1 == '*') {
			emit(&e, i, 2, HC_COM);
			i += 2;
			st = ST_CBLOCK;
			continue;
		}
		if ((lang == HL_C || lang == HL_JSON) && c == '/' && c1 == '/') {
			emit(&e, i, n - i, HC_COM);
			break;
		}
		if ((lang == HL_PY || lang == HL_SH) && c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
			emit(&e, i, n - i, HC_COM);
			break;
		}
		if ((lang == HL_LUA || lang == HL_SQL) && c == '-' && c1 == '-') {
			if (lang == HL_LUA && i + 3 < n && s[i + 2] == '[' && s[i + 3] == '[') {
				emit(&e, i, 4, HC_COM);
				i += 4;
				st = ST_LUABLOCK;
				continue;
			}
			emit(&e, i, n - i, HC_COM);
			break;
		}
		if (lang == HL_HTML && !strncmp(s + i, "<!--", 4)) {
			emit(&e, i, 4, HC_COM);
			i += 4;
			st = ST_HTMLCOM;
			continue;
		}
		if (lang == HL_HTML && c == '<') {
			i = tag(&e, s, n, i);
			continue;
		}
		if (lang == HL_MD && c == '`') {
			const char *q = strchr(s + i + 1, '`');
			int end = q ? (int)(q - s) + 1 : n;
			emit(&e, i, end - i, HC_STR);
			i = end;
			continue;
		}
		/* textos entre comillas (en Python, también los de tres comillas) */
		if (lang == HL_PY && (c == '"' || c == '\'') && c1 == c && i + 2 < n && s[i + 2] == c) {
			emit(&e, i, 3, HC_STR);
			i += 3;
			st = c == '"' ? ST_PY3D : ST_PY3S;
			continue;
		}
		if (lang != HL_HTML && lang != HL_MD && (c == '"' || c == '\'' || (c == '`' && lang == HL_C))) {
			int j = str_end(s, n, i), col = HC_STR;
			if (lang == HL_JSON && s[skip_sp(s, j)] == ':')
				col = HC_ATTR; /* clave */
			emit(&e, i, j - i, col);
			i = j;
			continue;
		}
		/* números y colores de CSS */
		if ((isdigit((unsigned char)c) && (i == 0 || !idch(s[i - 1]))) || (lang == HL_CSS && c == '#' && isxdigit((unsigned char)c1))) {
			int j = i + 1;
			while (j < n && (idch(s[j]) || s[j] == '.'))
				j++;
			emit(&e, i, j - i, HC_NUM);
			i = j;
			continue;
		}
		/* palabras: clave, función (seguida de «(»), propiedad de CSS o variable de shell */
		if (isalpha((unsigned char)c) || c == '_' || (lang == HL_SH && c == '$')) {
			int j = i + 1;
			while (j < n && (idch(s[j]) || (lang == HL_CSS && s[j] == '-')))
				j++;
			int col = HC_TEXT, k = skip_sp(s, j);
			if (lang != HL_MD && is_kw(lang, s + i, j - i))
				col = HC_KEY;
			else if (lang == HL_SH && c == '$')
				col = HC_ATTR;
			else if (lang == HL_CSS && s[k] == ':')
				col = HC_ATTR;
			else if ((lang == HL_C || lang == HL_PY || lang == HL_LUA) && s[k] == '(')
				col = HC_TAG;
			emit(&e, i, j - i, col);
			i = j;
			continue;
		}
		emit(&e, i, 1, HC_TEXT);
		i++;
	}
	*state = st;
	return e.n;
}
