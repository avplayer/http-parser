/* Based on src/http/ngx_http_parse.c from NGINX copyright Igor Sysoev
 *
 * Additional changes are licensed under the same terms as NGINX and
 * copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <stddef.h>
#include <limits.h>
#include <stdlib.h>

#include <limits>

#include "http_parser.hpp"

// #ifndef INT64_MAX
// # define INT64_MAX std::numeric_limits<int64_t>::max()
// #endif

#if HTTP_PARSER_DEBUG
#define SET_ERRNO(e)                                                 \
do {                                                                 \
  this->http_errno = (e);                                          \
  this->error_lineno = __LINE__;                                   \
} while (0)
#else
#define SET_ERRNO(e)                                                 \
do {                                                                 \
  this->m_http_errno = (e);                                          \
} while(0)
#endif

#define RETURN(r)                                                    \
do {                                                                 \
  this->state = state;                                             \
  return (r);                                                        \
} while(0)

/* Run the notify callback FOR, returning ER if it fails */
#define _CALLBACK_NOTIFY(FOR, ER)                                    \
do {                                                                 \
  this->state = state;                                             \
  assert(m_http_errno == HPE_OK);                       \
                                                                     \
  if (0 != settings.on_##FOR(*this)) {                             \
    SET_ERRNO(HPE_CB_##FOR);                                         \
  }                                                                  \
                                                                     \
  /* We either errored above or got paused; get out */               \
  if (m_http_errno != HPE_OK) {                         \
    return (ER);                                                     \
  }                                                                  \
} while (0)

/* Run the notify callback FOR and consume the current byte */
#define CALLBACK_NOTIFY(FOR)            _CALLBACK_NOTIFY(FOR, p - data + 1)

/* Run the notify callback FOR and don't consume the current byte */
#define CALLBACK_NOTIFY_NOADVANCE(FOR)  _CALLBACK_NOTIFY(FOR, p - data)

/* Run data callback FOR with LEN bytes, returning ER if it fails */
#define _CALLBACK_DATA(FOR, LEN, ER)                                 \
do {                                                                 \
  this->state = state;                                             \
  assert(m_http_errno == HPE_OK);                       \
                                                                     \
  if (FOR##_mark) {                                                  \
    if (0 != settings.on_##FOR(*this, FOR##_mark, (LEN))) {        \
      SET_ERRNO(HPE_CB_##FOR);                                       \
    }                                                                \
                                                                     \
    /* We either errored above or got paused; get out */             \
    if (m_http_errno != HPE_OK) {                       \
      return (ER);                                                   \
    }                                                                \
    FOR##_mark = nullptr;                                               \
  }                                                                  \
} while (0)

/* Run the data callback FOR and consume the current byte */
#define CALLBACK_DATA(FOR)                                           \
    _CALLBACK_DATA(FOR, p - FOR##_mark, p - data + 1)

/* Run the data callback FOR and don't consume the current byte */
#define CALLBACK_DATA_NOADVANCE(FOR)                                 \
    _CALLBACK_DATA(FOR, p - FOR##_mark, p - data)

/* We just saw a synthetic space */
#define CALLBACK_SPACE(FOR)                                          \
do {                                                                 \
  this->state = state;                                             \
  if (0 != settings.on_##FOR(*this, SPACE, 1)) {                   \
    SET_ERRNO(HPE_CB_##FOR);                                         \
    return (p - data);                                               \
  }                                                                  \
                                                                     \
  /* We either errored above or got paused; get out */               \
  if (m_http_errno != HPE_OK) {                         \
    return (p - data);                                               \
  }                                                                  \
} while (0)

/* Set the mark FOR; non-destructive if mark is already set */
#define MARK(FOR)                                                    \
do {                                                                 \
  if (!FOR##_mark) {                                                 \
    FOR##_mark = p;                                                  \
  }                                                                  \
} while (0)


#define CONTENT_LENGTH "content-length"
#define TRANSFER_ENCODING "transfer-encoding"
#define UPGRADE "upgrade"
#define CHUNKED "chunked"
#define SPACE " "


static const char *method_strings[] =
  { "DELETE"
  , "GET"
  , "HEAD"
  , "POST"
  , "PUT"
  , "CONNECT"
  , "OPTIONS"
  , "TRACE"
  , "COPY"
  , "LOCK"
  , "MKCOL"
  , "MOVE"
  , "PROPFIND"
  , "PROPPATCH"
  , "UNLOCK"
  , "REPORT"
  , "MKACTIVITY"
  , "CHECKOUT"
  , "MERGE"
  , "M-SEARCH"
  , "NOTIFY"
  , "SUBSCRIBE"
  , "UNSUBSCRIBE"
  , "PATCH"
  };


/* Tokens as defined by rfc 2616. Also lowercases them.
 *        token       = 1*<any CHAR except CTLs or separators>
 *     separators     = "(" | ")" | "<" | ">" | "@"
 *                    | "," | ";" | ":" | "\" | <">
 *                    | "/" | "[" | "]" | "?" | "="
 *                    | "{" | "}" | SP | HT
 */
static const char tokens[256] = {
/*   0 nul    1 soh    2 stx    3 etx    4 eot    5 enq    6 ack    7 bel  */
        0,       0,       0,       0,       0,       0,       0,       0,
/*   8 bs     9 ht    10 nl    11 vt    12 np    13 cr    14 so    15 si   */
        0,       0,       0,       0,       0,       0,       0,       0,
/*  16 dle   17 dc1   18 dc2   19 dc3   20 dc4   21 nak   22 syn   23 etb */
        0,       0,       0,       0,       0,       0,       0,       0,
/*  24 can   25 em    26 sub   27 esc   28 fs    29 gs    30 rs    31 us  */
        0,       0,       0,       0,       0,       0,       0,       0,
/*  32 sp    33  !    34  "    35  #    36  $    37  %    38  &    39  '  */
       ' ',      '!',     '"',     '#',     '$',     '%',     '&',    '\'',
/*  40  (    41  )    42  *    43  +    44  ,    45  -    46  .    47  /  */
        0,       0,      '*',     '+',      0,      '-',     '.',     '/',
/*  48  0    49  1    50  2    51  3    52  4    53  5    54  6    55  7  */
       '0',     '1',     '2',     '3',     '4',     '5',     '6',     '7',
/*  56  8    57  9    58  :    59  ;    60  <    61  =    62  >    63  ?  */
       '8',     '9',      0,       0,       0,       0,       0,       0,
/*  64  @    65  A    66  B    67  C    68  D    69  E    70  F    71  G  */
        0,      'a',     'b',     'c',     'd',     'e',     'f',     'g',
/*  72  H    73  I    74  J    75  K    76  L    77  M    78  N    79  O  */
       'h',     'i',     'j',     'k',     'l',     'm',     'n',     'o',
/*  80  P    81  Q    82  R    83  S    84  T    85  U    86  V    87  W  */
       'p',     'q',     'r',     's',     't',     'u',     'v',     'w',
/*  88  X    89  Y    90  Z    91  [    92  \    93  ]    94  ^    95  _  */
       'x',     'y',     'z',      0,       0,       0,      '^',     '_',
/*  96  `    97  a    98  b    99  c   100  d   101  e   102  f   103  g  */
       '`',     'a',     'b',     'c',     'd',     'e',     'f',     'g',
/* 104  h   105  i   106  j   107  k   108  l   109  m   110  n   111  o  */
       'h',     'i',     'j',     'k',     'l',     'm',     'n',     'o',
/* 112  p   113  q   114  r   115  s   116  t   117  u   118  v   119  w  */
       'p',     'q',     'r',     's',     't',     'u',     'v',     'w',
/* 120  x   121  y   122  z   123  {   124  |   125  }   126  ~   127 del */
       'x',     'y',     'z',      0,      '|',     '}',     '~',       0 };


static const int8_t unhex[256] =
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  , 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,-1,-1,-1,-1,-1,-1
  ,-1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  };

#if HTTP_PARSER_STRICT
# define T(v) 0
#else
# define T(v) v
#endif

static const uint8_t normal_url_char[256] = {
/*   0 nul    1 soh    2 stx    3 etx    4 eot    5 enq    6 ack    7 bel  */
        0,       0,       0,       0,       0,       0,       0,       0,
/*   8 bs     9 ht    10 nl    11 vt    12 np    13 cr    14 so    15 si   */
        0,     T(1),      0,       0,     T(1),      0,       0,       0,
/*  16 dle   17 dc1   18 dc2   19 dc3   20 dc4   21 nak   22 syn   23 etb */
        0,       0,       0,       0,       0,       0,       0,       0,
/*  24 can   25 em    26 sub   27 esc   28 fs    29 gs    30 rs    31 us  */
        0,       0,       0,       0,       0,       0,       0,       0,
/*  32 sp    33  !    34  "    35  #    36  $    37  %    38  &    39  '  */
        0,       1,       1,       0,       1,       1,       1,       1,
/*  40  (    41  )    42  *    43  +    44  ,    45  -    46  .    47  /  */
        1,       1,       1,       1,       1,       1,       1,       1,
/*  48  0    49  1    50  2    51  3    52  4    53  5    54  6    55  7  */
        1,       1,       1,       1,       1,       1,       1,       1,
/*  56  8    57  9    58  :    59  ;    60  <    61  =    62  >    63  ?  */
        1,       1,       1,       1,       1,       1,       1,       0,
/*  64  @    65  A    66  B    67  C    68  D    69  E    70  F    71  G  */
        1,       1,       1,       1,       1,       1,       1,       1,
/*  72  H    73  I    74  J    75  K    76  L    77  M    78  N    79  O  */
        1,       1,       1,       1,       1,       1,       1,       1,
/*  80  P    81  Q    82  R    83  S    84  T    85  U    86  V    87  W  */
        1,       1,       1,       1,       1,       1,       1,       1,
/*  88  X    89  Y    90  Z    91  [    92  \    93  ]    94  ^    95  _  */
        1,       1,       1,       1,       1,       1,       1,       1,
/*  96  `    97  a    98  b    99  c   100  d   101  e   102  f   103  g  */
        1,       1,       1,       1,       1,       1,       1,       1,
/* 104  h   105  i   106  j   107  k   108  l   109  m   110  n   111  o  */
        1,       1,       1,       1,       1,       1,       1,       1,
/* 112  p   113  q   114  r   115  s   116  t   117  u   118  v   119  w  */
        1,       1,       1,       1,       1,       1,       1,       1,
/* 120  x   121  y   122  z   123  {   124  |   125  }   126  ~   127 del */
        1,       1,       1,       1,       1,       1,       1,       0, };

#undef T

enum state
  { s_dead = 1 /* important that this is > 0 */
  , s_pre_start_req_or_res
  , s_start_req_or_res
  , s_res_or_resp_H

  , s_pre_start_res
  , s_start_res
  , s_res_H
  , s_res_HT
  , s_res_HTT
  , s_res_HTTP
  , s_res_first_http_major
  , s_res_http_major
  , s_res_first_http_minor
  , s_res_http_minor
  , s_res_first_status_code
  , s_res_status_code
  , s_res_status_start
  , s_res_status
  , s_res_line_almost_done

  , s_pre_start_req
  , s_start_req
  , s_req_method
  , s_req_spaces_before_url
  , s_req_schema
  , s_req_schema_slash
  , s_req_schema_slash_slash
  , s_req_server_start
  , s_req_server
  , s_req_server_with_at
  , s_req_host_start
  , s_req_host
  , s_req_host_ipv6
  , s_req_host_done
  , s_req_port
  , s_req_path
  , s_req_query_string_start
  , s_req_query_string
  , s_req_fragment_start
  , s_req_fragment
  , s_req_http_start
  , s_req_http_H
  , s_req_http_HT
  , s_req_http_HTT
  , s_req_http_HTTP
  , s_req_first_http_major
  , s_req_http_major
  , s_req_first_http_minor
  , s_req_http_minor
  , s_req_line_almost_done

  , s_header_field_start
  , s_header_field
  , s_header_value_start
  , s_header_value
  , s_header_value_lws

  , s_header_almost_done

  , s_chunk_size_start
  , s_chunk_size
  , s_chunk_parameters
  , s_chunk_size_almost_done

  , s_headers_almost_done
  , s_headers_done

  /* Important: 's_headers_done' must be the last 'header' state. All
   * states beyond this must be 'body' states. It is used for overflow
   * checking. See the PARSING_HEADER() macro.
   */

  , s_chunk_data
  , s_chunk_data_almost_done
  , s_chunk_data_done

  , s_body_identity
  , s_body_identity_eof

  , s_message_done
  };


#define PARSING_HEADER(state) (state <= s_headers_done)


enum header_states
  { h_general = 0

  , h_general_and_quote
  , h_general_and_quote_and_escape

  , h_matching_content_length
  , h_matching_transfer_encoding
  , h_matching_upgrade

  , h_content_length
  , h_transfer_encoding
  , h_upgrade

  , h_matching_transfer_encoding_chunked

  , h_transfer_encoding_chunked
  };

enum http_host_state
  {
    s_http_host_dead = 1
  , s_http_userinfo_start
  , s_http_userinfo
  , s_http_host_start
  , s_http_host_v6_start
  , s_http_host
  , s_http_host_v6
  , s_http_host_v6_end
  , s_http_host_port_start
  , s_http_host_port
};


/* Macros for character classes; depends on strict-mode  */
#define CR                  '\r'
#define LF                  '\n'
#define QT                  '"'
#define BS                  '\\'
#define LOWER(c)            (unsigned char)(c | 0x20)
#define TOKEN(c)            (tokens[(unsigned char)c])
#define IS_ALPHA(c)         (LOWER(c) >= 'a' && LOWER(c) <= 'z')
#define IS_NUM(c)           ((c) >= '0' && (c) <= '9')
#define IS_ALPHANUM(c)      (IS_ALPHA(c) || IS_NUM(c))
#define IS_HEX(c)           (IS_NUM(c) || (LOWER(c) >= 'a' && LOWER(c) <= 'f'))
#define IS_MARK(c)          ((c) == '-' || (c) == '_' || (c) == '.' || \
  (c) == '!' || (c) == '~' || (c) == '*' || (c) == '\'' || (c) == '(' || \
  (c) == ')')
#define IS_USERINFO_CHAR(c) (IS_ALPHANUM(c) || IS_MARK(c) || (c) == '%' || \
  (c) == ';' || (c) == ':' || (c) == '&' || (c) == '=' || (c) == '+' || \
  (c) == '$' || (c) == ',')

#if HTTP_PARSER_STRICT
#define IS_URL_CHAR(c)      (normal_url_char[(unsigned char) (c)])
#define IS_HOST_CHAR(c)     (IS_ALPHANUM(c) || (c) == '.' || (c) == '-')
#else
#define IS_URL_CHAR(c)                                                         \
  (normal_url_char[(unsigned char) (c)] || ((c) & 0x80))
#define IS_HOST_CHAR(c)                                                        \
  (IS_ALPHANUM(c) || (c) == '.' || (c) == '-' || (c) == '_')
#endif


#define start_state (type == HTTP_REQUEST ? s_pre_start_req : s_pre_start_res)

#define STRICT_CHECK(cond)
#define NEW_MESSAGE() start_state

/* Map errno values to strings for human-readable output */
#define HTTP_STRERROR_GEN(n, s) { "HPE_" #n, s },
static struct {
  const char *name;
  const char *description;
} http_strerror_tab[] = {
  HTTP_ERRNO_MAP(HTTP_STRERROR_GEN)
};
#undef HTTP_STRERROR_GEN

/* Our URL parser.
 *
 * This is designed to be shared by http_parser_execute() for URL validation,
 * hence it has a state transition + byte-for-byte interface. In addition, it
 * is meant to be embedded in http_parser_parse_url(), which does the dirty
 * work of turning state transitions URL components for its API.
 *
 * This function should only be invoked with non-space characters. It is
 * assumed that the caller cares about (and can detect) the transition between
 * URL and non-URL states by looking for these.
 */
static enum state
parse_url_char(enum state s, const char ch)
{
  if (ch == ' ' || ch == '\r' || ch == '\n') {
    return s_dead;
  }

#if HTTP_PARSER_STRICT
  if (ch == '\t' || ch == '\f') {
    return s_dead;
  }
#endif

  switch (s) {
    case s_req_spaces_before_url:
      /* Proxied requests are followed by scheme of an absolute URI (alpha).
       * All methods except CONNECT are followed by '/' or '*'.
       */

      if (ch == '/' || ch == '*') {
        return s_req_path;
      }

      if (IS_ALPHA(ch)) {
        return s_req_schema;
      }

      break;

    case s_req_schema:
      if (IS_ALPHA(ch)) {
        return s;
      }

      if (ch == ':') {
        return s_req_schema_slash;
      }

      break;

    case s_req_schema_slash:
      if (ch == '/') {
        return s_req_schema_slash_slash;
      }

      break;

    case s_req_schema_slash_slash:
      if (ch == '/') {
        return s_req_server_start;
      }

      break;

    case s_req_server_with_at:
      if (ch == '@') {
        return s_dead;
      }

    /* FALLTHROUGH */
    case s_req_server_start:
    case s_req_server:
      if (ch == '/') {
        return s_req_path;
      }

      if (ch == '?') {
        return s_req_query_string_start;
      }

      if (ch == '@') {
        return s_req_server_with_at;
      }

      if (IS_USERINFO_CHAR(ch) || ch == '[' || ch == ']') {
        return s_req_server;
      }

      break;

    case s_req_path:
      if (IS_URL_CHAR(ch)) {
        return s;
      }

      switch (ch) {
        case '?':
          return s_req_query_string_start;

        case '#':
          return s_req_fragment_start;
      }

      break;

    case s_req_query_string_start:
    case s_req_query_string:
      if (IS_URL_CHAR(ch)) {
        return s_req_query_string;
      }

      switch (ch) {
        case '?':
          /* allow extra '?' in query string */
          return s_req_query_string;

        case '#':
          return s_req_fragment_start;
      }

      break;

    case s_req_fragment_start:
      if (IS_URL_CHAR(ch)) {
        return s_req_fragment;
      }

      switch (ch) {
        case '?':
          return s_req_fragment;

        case '#':
          return s;
      }

      break;

    case s_req_fragment:
      if (IS_URL_CHAR(ch)) {
        return s;
      }

      switch (ch) {
        case '?':
        case '#':
          return s;
      }

      break;

    default:
      break;
  }

  /* We should never fall out of the switch above unless there's an error */
  return s_dead;
}

static inline http_host_state http_parse_host_char(http_host_state s, const char ch)
{
  switch(s) {
    case s_http_userinfo:
    case s_http_userinfo_start:
      if (ch == '@') {
        return s_http_host_start;
      }

      if (IS_USERINFO_CHAR(ch)) {
        return s_http_userinfo;
      }
      break;

    case s_http_host_start:
      if (ch == '[') {
        return s_http_host_v6_start;
      }

      if (IS_HOST_CHAR(ch)) {
        return s_http_host;
      }

      break;

    case s_http_host:
      if (IS_HOST_CHAR(ch)) {
        return s_http_host;
      }

    /* FALLTHROUGH */
    case s_http_host_v6_end:
      if (ch == ':') {
        return s_http_host_port_start;
      }

      break;

    case s_http_host_v6:
      if (ch == ']') {
        return s_http_host_v6_end;
      }

    /* FALLTHROUGH */
    case s_http_host_v6_start:
      if (IS_HEX(ch) || ch == ':' || ch == '.') {
        return s_http_host_v6;
      }

      break;

    case s_http_host_port:
    case s_http_host_port_start:
      if (IS_NUM(ch)) {
        return s_http_host_port;
      }

      break;

    default:
      break;
  }
  return s_http_host_dead;
}

static inline int http_parse_host(const char * buf, struct http_parser_url *u, int found_at)
{
  enum http_host_state s;

  const char *p;
  size_t buflen = u->field_data[UF_HOST].off + u->field_data[UF_HOST].len;

  u->field_data[UF_HOST].len = 0;

  s = found_at ? s_http_userinfo_start : s_http_host_start;

  for (p = buf + u->field_data[UF_HOST].off; p < buf + buflen; p++) {
    enum http_host_state new_s = http_parse_host_char(s, *p);

    if (new_s == s_http_host_dead) {
      return 1;
    }

    switch(new_s) {
      case s_http_host:
        if (s != s_http_host) {
          u->field_data[UF_HOST].off = p - buf;
        }
        u->field_data[UF_HOST].len++;
        break;

      case s_http_host_v6:
        if (s != s_http_host_v6) {
          u->field_data[UF_HOST].off = p - buf;
        }
        u->field_data[UF_HOST].len++;
        break;

      case s_http_host_port:
        if (s != s_http_host_port) {
          u->field_data[UF_PORT].off = p - buf;
          u->field_data[UF_PORT].len = 0;
          u->field_set |= (1 << UF_PORT);
        }
        u->field_data[UF_PORT].len++;
        break;

      case s_http_userinfo:
        if (s != s_http_userinfo) {
          u->field_data[UF_USERINFO].off = p - buf ;
          u->field_data[UF_USERINFO].len = 0;
          u->field_set |= (1 << UF_USERINFO);
        }
        u->field_data[UF_USERINFO].len++;
        break;

      default:
        break;
    }
    s = new_s;
  }

  /* Make sure we don't end somewhere unexpected */
  switch (s) {
    case s_http_host_start:
    case s_http_host_v6_start:
    case s_http_host_v6:
    case s_http_host_port_start:
    case s_http_userinfo:
    case s_http_userinfo_start:
      return 1;
    default:
      break;
  }

  return 0;
}


int
http_parser_parse_url(const char *buf, size_t buflen, int is_connect,
                      struct http_parser_url *u)
{
  enum state s;
  const char *p;
  enum http_parser_url_fields uf, old_uf;
  int found_at = 0;

  u->port = u->field_set = 0;
  s = is_connect ? s_req_server_start : s_req_spaces_before_url;
  uf = old_uf = UF_MAX;

  for (p = buf; p < buf + buflen; p++) {
    s = parse_url_char(s, *p);

    /* Figure out the next field that we're operating on */
    switch (s) {
      case s_dead:
        return 1;

      /* Skip delimeters */
      case s_req_schema_slash:
      case s_req_schema_slash_slash:
      case s_req_server_start:
      case s_req_query_string_start:
      case s_req_fragment_start:
        continue;

      case s_req_schema:
        uf = UF_SCHEMA;
        break;

      case s_req_server_with_at:
        found_at = 1;

      /* FALLTROUGH */
      case s_req_server:
        uf = UF_HOST;
        break;

      case s_req_path:
        uf = UF_PATH;
        break;

      case s_req_query_string:
        uf = UF_QUERY;
        break;

      case s_req_fragment:
        uf = UF_FRAGMENT;
        break;

      default:
        assert(!"Unexpected state");
        return 1;
    }

    /* Nothing's changed; soldier on */
    if (uf == old_uf) {
      u->field_data[uf].len++;
      continue;
    }

    u->field_data[uf].off = p - buf;
    u->field_data[uf].len = 1;

    u->field_set |= (1 << uf);
    old_uf = uf;
  }

  /* host must be present if there is a schema */
  /* parsing http:///toto will fail */
  if ((u->field_set & ((1 << UF_SCHEMA) | (1 << UF_HOST))) != 0) {
    if (http_parse_host(buf, u, found_at) != 0) {
      return 1;
    }
  }

  /* CONNECT requests can only contain "hostname:port" */
  if (is_connect && u->field_set != ((1 << UF_HOST)|(1 << UF_PORT))) {
    return 1;
  }

  if (u->field_set & (1 << UF_PORT)) {
    /* Don't bother with endp; we've already validated the string */
    unsigned long v = strtoul(buf + u->field_data[UF_PORT].off, nullptr, 10);

    /* Ports have a max value of 2^16 */
    if (v > 0xffff) {
      return 1;
    }

    u->port = (uint16_t) v;
  }

  return 0;
}


http_parser::http_parser(http_parser_type t)
{
    this->type = t;
    this->state = (t == HTTP_REQUEST ? s_pre_start_req : (t == HTTP_RESPONSE ? s_pre_start_res : s_pre_start_req_or_res));
    this->nread = 0;
    this->m_upgrade = 0;
    this->flags = 0;
    this->m_method = 0;
    this->m_http_errno = HPE_OK;
}

std::size_t http_parser::execute(const parser_settings& settings, const char *data, size_t len)
{
	char c, ch;
	int8_t unhex_val;
	const char *p = data;

	/* Optimization: within the parsing loop below, we refer to this
	* local copy of the state rather than state.  The compiler
	* can't be sure whether state will change during a callback,
	* so it generates a lot of memory loads and stores to keep a register
	* copy of the state in sync with the memory copy.  We know, however,
	* that the callbacks aren't allowed to change the parser state, so
	* the parsing loop works with this local variable and only copies
	* the value back to loop before returning or invoking a
	* callback.
	*/
	unsigned char state = this->state;

	/* We're in an error state. Don't bother doing anything. */
	if (m_http_errno != HPE_OK)
	{
		RETURN(0);
	}

	if (len == 0)
	{
		switch (state)
		{
		case s_body_identity_eof:
			/* Use of CALLBACK_NOTIFY() here would erroneously return 1 byte read if
			* we got paused.
			*/
			CALLBACK_NOTIFY_NOADVANCE(message_complete);
			RETURN(0);

		case s_pre_start_req_or_res:
		case s_pre_start_res:
		case s_pre_start_req:
			RETURN(0);

		default:
			SET_ERRNO(HPE_INVALID_EOF_STATE);
			RETURN(1);
		}
	}

	/* technically we could combine all of these (except for url_mark) into one
	variable, saving stack space, but it seems more clear to have them
	separated. */
	const char *header_field_mark = 0;
	const char *header_value_mark = 0;
	const char *url_mark = 0;
	const char *reason_mark = 0;
	const char *body_mark = 0;

	if (state == s_header_field)
		header_field_mark = data;
	if (state == s_header_value)
		header_value_mark = data;
	if (state == s_req_path ||
			state == s_req_schema ||
			state == s_req_schema_slash ||
			state == s_req_schema_slash_slash ||
			state == s_req_port ||
			state == s_req_query_string_start ||
			state == s_req_query_string ||
			state == s_req_host_start ||
			state == s_req_host ||
			state == s_req_host_ipv6 ||
			state == s_req_host_done ||
			state == s_req_fragment_start ||
			state == s_req_fragment)
		url_mark = data;
	if (state == s_res_status)
		reason_mark = data;

	/* Used only for overflow checking. If the parser is in a parsing-headers
	* state, then its value is equal to max(data, the beginning of the current
	* message or chunk). If the parser is in a not-parsing-headers state, then
	* its value is irrelevant.
	*/
	const char* data_or_header_data_start = data;

	for (p = data; p != data + len; p++) {
		ch = *p;

reexecute_byte:
		switch (state) {

		case s_pre_start_req_or_res:
			if (ch == CR || ch == LF)
				break;
			state = s_start_req_or_res;
			CALLBACK_NOTIFY_NOADVANCE(message_begin);
			goto reexecute_byte;

		case s_start_req_or_res:
		{
			flags = 0;
			m_content_length = -1;

			if (ch == 'H') {
				state = s_res_or_resp_H;
			} else {
				type = HTTP_REQUEST;
				state = s_start_req;
				goto reexecute_byte;
			}

			break;
		}

		case s_res_or_resp_H:
			if (ch == 'T') {
				type = HTTP_RESPONSE;
				state = s_res_HT;
			} else {
				if (ch != 'E') {
					SET_ERRNO(HPE_INVALID_CONSTANT);
					goto error;
				}

				type = HTTP_REQUEST;
				m_method = HTTP_HEAD;
				index = 2;
				state = s_req_method;
			}
			break;

		case s_pre_start_res:
			if (ch == CR || ch == LF)
				break;
			state = s_start_res;
			CALLBACK_NOTIFY_NOADVANCE(message_begin);
			goto reexecute_byte;

		case s_start_res:
		{
			flags = 0;
			m_content_length = -1;

			switch (ch) {
			case 'H':
				state = s_res_H;
				break;

			default:
				SET_ERRNO(HPE_INVALID_CONSTANT);
				goto error;
			}

			break;
		}

		case s_res_H:
			STRICT_CHECK(ch != 'T');
			state = s_res_HT;
			break;

		case s_res_HT:
			STRICT_CHECK(ch != 'T');
			state = s_res_HTT;
			break;

		case s_res_HTT:
			STRICT_CHECK(ch != 'P');
			state = s_res_HTTP;
			break;

		case s_res_HTTP:
			STRICT_CHECK(ch != '/');
			state = s_res_first_http_major;
			break;

		case s_res_first_http_major:
			if (ch < '0' || ch > '9') {
				SET_ERRNO(HPE_INVALID_VERSION);
				goto error;
			}

			m_http_major = ch - '0';
			state = s_res_http_major;
			break;

			/* major HTTP version or dot */
		case s_res_http_major:
		{
			if (ch == '.') {
				state = s_res_first_http_minor;
				break;
			}

			if (!IS_NUM(ch)) {
				SET_ERRNO(HPE_INVALID_VERSION);
				goto error;
			}

			m_http_major *= 10;
			m_http_major += ch - '0';

			if (m_http_major > 999) {
				SET_ERRNO(HPE_INVALID_VERSION);
				goto error;
			}

			break;
		}

		/* first digit of minor HTTP version */
		case s_res_first_http_minor:
			if (!IS_NUM(ch)) {
				SET_ERRNO(HPE_INVALID_VERSION);
				goto error;
			}

			m_http_minor = ch - '0';
			state = s_res_http_minor;
			break;

			/* minor HTTP version or end of request line */
		case s_res_http_minor:
		{
			if (ch == ' ') {
				state = s_res_first_status_code;
				break;
			}

			if (!IS_NUM(ch)) {
				SET_ERRNO(HPE_INVALID_VERSION);
				goto error;
			}

			m_http_minor *= 10;
			m_http_minor += ch - '0';

			if (m_http_minor > 999) {
				SET_ERRNO(HPE_INVALID_VERSION);
				goto error;
			}

			break;
		}

		case s_res_first_status_code:
		{
			if (!IS_NUM(ch)) {
				if (ch == ' ') {
					break;
				}

				SET_ERRNO(HPE_INVALID_STATUS);
				goto error;
			}
			m_status_code = ch - '0';
			state = s_res_status_code;
			break;
		}

		case s_res_status_code:
		{
			if (!IS_NUM(ch)) {
				switch (ch) {
				case ' ':
					state = s_res_status;
					break;
				case CR:
					state = s_res_line_almost_done;
					break;
				case LF:
					state = s_header_field_start;
					break;
				default:
					SET_ERRNO(HPE_INVALID_STATUS);
					goto error;
				}
				break;
			}

			m_status_code *= 10;
			m_status_code += ch - '0';

			if (m_status_code > 999) {
				SET_ERRNO(HPE_INVALID_STATUS);
				goto error;
			}

			break;
		}

		case s_res_status:
			/* the human readable status. e.g. "NOT FOUND" */
			MARK(reason);
			if (ch == CR) {
				state = s_res_line_almost_done;
				CALLBACK_DATA(reason);
				break;
			}

			if (ch == LF) {
				state = s_header_field_start;
				CALLBACK_DATA(reason);
				break;
			}
			break;

		case s_res_line_almost_done:
			STRICT_CHECK(ch != LF);
			state = s_header_field_start;
			break;

		case s_pre_start_req:
			if (ch == CR || ch == LF) {
				break;
			}
			state = s_start_req;
			CALLBACK_NOTIFY_NOADVANCE(message_begin);
			goto reexecute_byte;

		case s_start_req:
		{
			flags = 0;
			m_content_length = -1;

			if (!IS_ALPHA(ch)) {
				SET_ERRNO(HPE_INVALID_METHOD);
				goto error;
			}

			m_method = (enum http_method) 0;
			index = 1;
			switch (ch) {
			case 'C':
				m_method = HTTP_CONNECT; /* or COPY, CHECKOUT */ break;
			case 'D':
				m_method = HTTP_DELETE;
				break;
			case 'G':
				m_method = HTTP_GET;
				break;
			case 'H':
				m_method = HTTP_HEAD;
				break;
			case 'L':
				m_method = HTTP_LOCK;
				break;
			case 'M':
				m_method = HTTP_MKCOL; /* or MOVE, MKACTIVITY, MERGE, M-SEARCH */ break;
			case 'N':
				m_method = HTTP_NOTIFY;
				break;
			case 'O':
				m_method = HTTP_OPTIONS;
				break;
			case 'P':
				m_method = HTTP_POST;
				/* or PROPFIND or PROPPATCH or PUT or PATCH */
				break;
			case 'R':
				m_method = HTTP_REPORT;
				break;
			case 'S':
				m_method = HTTP_SUBSCRIBE;
				break;
			case 'T':
				m_method = HTTP_TRACE;
				break;
			case 'U':
				m_method = HTTP_UNLOCK; /* or UNSUBSCRIBE */ break;
			default:
				SET_ERRNO(HPE_INVALID_METHOD);
				goto error;
			}
			state = s_req_method;

			break;
		}

		case s_req_method:
		{
			if (ch == '\0') {
				SET_ERRNO(HPE_INVALID_METHOD);
				goto error;
			}

			const char *matcher = method_strings[m_method];
			if (ch == ' ' && matcher[index] == '\0') {
				state = s_req_spaces_before_url;
			} else if (ch == matcher[index]) {
				; /* nada */
			} else if (m_method == HTTP_CONNECT) {
				if (index == 1 && ch == 'H') {
					m_method = HTTP_CHECKOUT;
				} else if (index == 2  && ch == 'P') {
					m_method = HTTP_COPY;
				} else {
					goto error;
				}
			} else if (m_method == HTTP_MKCOL) {
				if (index == 1 && ch == 'O') {
					m_method = HTTP_MOVE;
				} else if (index == 1 && ch == 'E') {
					m_method = HTTP_MERGE;
				} else if (index == 1 && ch == '-') {
					m_method = HTTP_MSEARCH;
				} else if (index == 2 && ch == 'A') {
					m_method = HTTP_MKACTIVITY;
				} else {
					goto error;
				}
			} else if (index == 1 && m_method == HTTP_POST) {
				if (ch == 'R') {
					m_method = HTTP_PROPFIND; /* or HTTP_PROPPATCH */
				} else if (ch == 'U') {
					m_method = HTTP_PUT;
				} else if (ch == 'A') {
					m_method = HTTP_PATCH;
				} else {
					goto error;
				}
			} else if (index == 2 && m_method == HTTP_UNLOCK && ch == 'S') {
				m_method = HTTP_UNSUBSCRIBE;
			} else if (index == 4 && m_method == HTTP_PROPFIND && ch == 'P') {
				m_method = HTTP_PROPPATCH;
			} else {
				SET_ERRNO(HPE_INVALID_METHOD);
				goto error;
			}

			++index;
			break;
		}

		case s_req_spaces_before_url:
		{
			if (ch == ' ') break;

			// CONNECT requests must be followed by a <host>:<port>
			if (m_method == HTTP_CONNECT) {
				MARK(url);
				state = s_req_host_start;
				goto reexecute_byte;
			}

			if (ch == '/' || ch == '*') {
				MARK(url);
				state = s_req_path;
				break;
			}

			/* Proxied requests are followed by scheme of an absolute URI (alpha).
			* All other methods are followed by '/' or '*' (handled above).
			*/
			if (IS_ALPHA(ch)) {
				MARK(url);
				state = s_req_schema;
				break;
			}

			SET_ERRNO(HPE_INVALID_URL);
			goto error;
		}

		case s_req_schema:
		{
			if (IS_ALPHA(ch)) break;

			if (ch == ':') {
				state = s_req_schema_slash;
				break;
			}

			SET_ERRNO(HPE_INVALID_URL);
			goto error;
		}

		case s_req_schema_slash:
			STRICT_CHECK(ch != '/');
			state = s_req_schema_slash_slash;
			break;

		case s_req_schema_slash_slash:
			STRICT_CHECK(ch != '/');
			state = s_req_host_start;
			break;

		case s_req_host_start:
			if (ch == '[') {
				state = s_req_host_ipv6;
				break;
			} else if (IS_ALPHANUM(ch)) {
				state = s_req_host;
				break;
			}

			SET_ERRNO(HPE_INVALID_HOST);
			goto error;

		case s_req_host:
			if (IS_HOST_CHAR(ch)) break;
			state = s_req_host_done;
			goto reexecute_byte;

		case s_req_host_ipv6:
			if (IS_HEX(ch) || ch == ':') break;
			if (ch == ']') {
				state = s_req_host_done;
				break;
			}

			SET_ERRNO(HPE_INVALID_HOST);
			goto error;

		case s_req_host_done:
			switch (ch) {
			case ':':
				state = s_req_port;
				break;
			case '/':
				state = s_req_path;
				break;
			case ' ':
				/* The request line looks like:
				*   "GET http://foo.bar.com HTTP/1.1"
				* That is, there is no path.
				*/
				state = s_req_http_start;
				CALLBACK_DATA(url);
				break;
			case '?':
				state = s_req_query_string_start;
				break;
			default:
				SET_ERRNO(HPE_INVALID_HOST);
				goto error;
			}

			break;

		case s_req_port:
		{
			if (IS_NUM(ch)) break;
			switch (ch) {
			case '/':
				state = s_req_path;
				break;
			case ' ':
				/* The request line looks like:
				*   "GET http://foo.bar.com:1234 HTTP/1.1"
				* That is, there is no path.
				*/
				state = s_req_http_start;
				CALLBACK_DATA(url);
				break;
			case '?':
				state = s_req_query_string_start;
				break;
			default:
				SET_ERRNO(HPE_INVALID_PORT);
				goto error;
			}
			break;
		}

		case s_req_path:
		{
			if (IS_URL_CHAR(ch)) break;

			switch (ch) {
			case ' ':
				state = s_req_http_start;
				CALLBACK_DATA(url);
				break;
			case CR:
				m_http_major = 0;
				m_http_minor = 9;
				state = s_req_line_almost_done;
				CALLBACK_DATA(url);
				break;
			case LF:
				m_http_major = 0;
				m_http_minor = 9;
				state = s_header_field_start;
				CALLBACK_DATA(url);
				break;
			case '?':
				state = s_req_query_string_start;
				break;
			case '#':
				state = s_req_fragment_start;
				break;
			default:
				SET_ERRNO(HPE_INVALID_PATH);
				goto error;
			}
			break;
		}

		case s_req_query_string_start:
		{
			if (IS_URL_CHAR(ch)) {
				state = s_req_query_string;
				break;
			}

			switch (ch) {
			case '?':
				break; /* XXX ignore extra '?' ... is this right? */
			case ' ':
				state = s_req_http_start;
				CALLBACK_DATA(url);
				break;
			case CR:
				m_http_major = 0;
				m_http_minor = 9;
				state = s_req_line_almost_done;
				CALLBACK_DATA(url);
				break;
			case LF:
				m_http_major = 0;
				m_http_minor = 9;
				state = s_header_field_start;
				CALLBACK_DATA(url);
				break;
			case '#':
				state = s_req_fragment_start;
				break;
			default:
				SET_ERRNO(HPE_INVALID_QUERY_STRING);
				goto error;
			}
			break;
		}

		case s_req_query_string:
		{
			if (IS_URL_CHAR(ch)) break;

			switch (ch) {
			case '?':
				/* allow extra '?' in query string */
				break;
			case ' ':
				state = s_req_http_start;
				CALLBACK_DATA(url);
				break;
			case CR:
				m_http_major = 0;
				m_http_minor = 9;
				state = s_req_line_almost_done;
				CALLBACK_DATA(url);
				break;
			case LF:
				m_http_major = 0;
				m_http_minor = 9;
				state = s_header_field_start;
				CALLBACK_DATA(url);
				break;
			case '#':
				state = s_req_fragment_start;
				break;
			default:
				SET_ERRNO(HPE_INVALID_QUERY_STRING);
				goto error;
			}
			break;
		}

		case s_req_fragment_start:
		{
			if (IS_URL_CHAR(ch)) {
				state = s_req_fragment;
				break;
			}

			switch (ch) {
			case ' ':
				state = s_req_http_start;
				CALLBACK_DATA(url);
				break;
			case CR:
				m_http_major = 0;
				m_http_minor = 9;
				state = s_req_line_almost_done;
				CALLBACK_DATA(url);
				break;
			case LF:
				m_http_major = 0;
				m_http_minor = 9;
				state = s_header_field_start;
				CALLBACK_DATA(url);
				break;
			case '?':
				state = s_req_fragment;
				break;
			case '#':
				break;
			default:
				SET_ERRNO(HPE_INVALID_FRAGMENT);
				goto error;
			}
			break;
		}

		case s_req_fragment:
		{
			if (IS_URL_CHAR(ch)) break;

			switch (ch) {
			case ' ':
				state = s_req_http_start;
				CALLBACK_DATA(url);
				break;
			case CR:
				m_http_major = 0;
				m_http_minor = 9;
				state = s_req_line_almost_done;
				CALLBACK_DATA(url);
				break;
			case LF:
				m_http_major = 0;
				m_http_minor = 9;
				state = s_header_field_start;
				CALLBACK_DATA(url);
				break;
			case '?':
			case '#':
				break;
			default:
				SET_ERRNO(HPE_INVALID_FRAGMENT);
				goto error;
			}
			break;
		}

		case s_req_http_start:
			switch (ch) {
			case 'H':
				state = s_req_http_H;
				break;
			case ' ':
				break;
			default:
				SET_ERRNO(HPE_INVALID_CONSTANT);
				goto error;
			}
			break;

		case s_req_http_H:
			STRICT_CHECK(ch != 'T');
			state = s_req_http_HT;
			break;

		case s_req_http_HT:
			STRICT_CHECK(ch != 'T');
			state = s_req_http_HTT;
			break;

		case s_req_http_HTT:
			STRICT_CHECK(ch != 'P');
			state = s_req_http_HTTP;
			break;

		case s_req_http_HTTP:
			STRICT_CHECK(ch != '/');
			state = s_req_first_http_major;
			break;

			/* first digit of major HTTP version */
		case s_req_first_http_major:
			if (ch < '0' || ch > '9') {
				SET_ERRNO(HPE_INVALID_VERSION);
				goto error;
			}

			m_http_major = ch - '0';
			state = s_req_http_major;
			break;

			/* major HTTP version or dot */
		case s_req_http_major:
		{
			if (ch == '.') {
				state = s_req_first_http_minor;
				break;
			}

			if (!IS_NUM(ch)) {
				SET_ERRNO(HPE_INVALID_VERSION);
				goto error;
			}

			m_http_major *= 10;
			m_http_major += ch - '0';

			if (m_http_major > 999) {
				SET_ERRNO(HPE_INVALID_VERSION);
				goto error;
			}

			break;
		}

		/* first digit of minor HTTP version */
		case s_req_first_http_minor:
			if (!IS_NUM(ch)) {
				SET_ERRNO(HPE_INVALID_VERSION);
				goto error;
			}

			m_http_minor = ch - '0';
			state = s_req_http_minor;
			break;

			/* minor HTTP version or end of request line */
		case s_req_http_minor:
		{
			if (ch == CR) {
				state = s_req_line_almost_done;
				break;
			}

			if (ch == LF) {
				state = s_header_field_start;
				break;
			}

			/* XXX allow spaces after digit? */

			if (!IS_NUM(ch)) {
				SET_ERRNO(HPE_INVALID_VERSION);
				goto error;
			}

			m_http_minor *= 10;
			m_http_minor += ch - '0';

			if (m_http_minor > 999) {
				SET_ERRNO(HPE_INVALID_VERSION);
				goto error;
			}

			break;
		}

		/* end of request line */
		case s_req_line_almost_done:
		{
			if (ch != LF) {
				SET_ERRNO(HPE_LF_EXPECTED);
				goto error;
			}

			state = s_header_field_start;
			break;
		}

		case s_header_field_start:
		{
			if (ch == CR) {
				state = s_headers_almost_done;
				break;
			}

			if (ch == LF) {
				/* they might be just sending \n instead of \r\n so this would be
				* the second \n to denote the end of headers*/
				state = s_headers_almost_done;
				goto reexecute_byte;
			}

			c = TOKEN(ch);

			if (!c) {
				SET_ERRNO(HPE_INVALID_HEADER_TOKEN);
				goto error;
			}

			MARK(header_field);

			index = 0;
			state = s_header_field;

			switch (c) {
			case 'c':
				header_state = h_matching_content_length;
				break;

			case 't':
				header_state = h_matching_transfer_encoding;
				break;

			case 'u':
				header_state = h_matching_upgrade;
				break;

			default:
				header_state = h_general;
				break;
			}
			break;
		}

		case s_header_field:
		{
			c = TOKEN(ch);

			if (c) {
				switch (header_state) {
				case h_general:

					// fast-forwarding, wheeeeeee!
#define MOVE_THE_HEAD do { \
			++p;                     \
			if (!TOKEN(*p)) {        \
			ch = *p;               \
			goto notatoken;        \
			}                        \
		} while(0);

					if (data + len - p >= 9) {
						MOVE_THE_HEAD
						MOVE_THE_HEAD
						MOVE_THE_HEAD
						MOVE_THE_HEAD
						MOVE_THE_HEAD
						MOVE_THE_HEAD
						MOVE_THE_HEAD
						MOVE_THE_HEAD
					} else if (data + len - p >= 4) {
						MOVE_THE_HEAD
						MOVE_THE_HEAD
						MOVE_THE_HEAD
					}

					break;

					/* content-length */

				case h_matching_content_length:
					index++;
					if (index > sizeof(CONTENT_LENGTH)-1
							|| c != CONTENT_LENGTH[index]) {
						header_state = h_general;
					} else if (index == sizeof(CONTENT_LENGTH)-2) {
						header_state = h_content_length;
					}
					break;

					/* transfer-encoding */

				case h_matching_transfer_encoding:
					index++;
					if (index > sizeof(TRANSFER_ENCODING)-1
							|| c != TRANSFER_ENCODING[index]) {
						header_state = h_general;
					} else if (index == sizeof(TRANSFER_ENCODING)-2) {
						header_state = h_transfer_encoding;
					}
					break;

					/* upgrade */

				case h_matching_upgrade:
					index++;
					if (index > sizeof(UPGRADE)-1
							|| c != UPGRADE[index]) {
						header_state = h_general;
					} else if (index == sizeof(UPGRADE)-2) {
						header_state = h_upgrade;
					}
					break;

				case h_content_length:
				case h_transfer_encoding:
				case h_upgrade:
					if (ch != ' ') header_state = h_general;
					break;

				default:
					assert(0 && "Unknown header_state");
					break;
				}
				break;
			}

notatoken:
			if (ch == ':') {
				state = s_header_value_start;
				CALLBACK_DATA(header_field);
				break;
			}

			SET_ERRNO(HPE_INVALID_HEADER_TOKEN);
			goto error;
		}

		case s_header_value_start:
		{
			if (ch == ' ' || ch == '\t') break;

			MARK(header_value);

			state = s_header_value;
			index = 0;

			if (ch == CR) {
				STRICT_CHECK(quote != 0);
				header_state = h_general;
				state = s_header_almost_done;
				CALLBACK_DATA(header_value);
				break;
			}

			if (ch == LF) {
				STRICT_CHECK(quote != 0);
				state = s_header_field_start;
				CALLBACK_DATA(header_value);
				break;
			}

			c = LOWER(ch);

			switch (header_state) {
			case h_upgrade:
				flags |= F_UPGRADE;
				header_state = h_general;
				break;

			case h_transfer_encoding:
				/* looking for 'Transfer-Encoding: chunked' */
				if ('c' == c) {
					header_state = h_matching_transfer_encoding_chunked;
				} else {
					header_state = h_general;
				}
				break;

			case h_content_length:
				if (!IS_NUM(ch)) {
					SET_ERRNO(HPE_INVALID_CONTENT_LENGTH);
					goto error;
				}

				m_content_length = ch - '0';
				break;

			default:
				header_state = ch == QT ? h_general_and_quote : h_general;
				break;
			}
			break;
		}

		case s_header_value:
		{
cr_or_lf_or_qt:
			if (ch == CR &&
					header_state != h_general_and_quote_and_escape) {
				state = s_header_almost_done;
				CALLBACK_DATA(header_value);
				break;
			}

			if (ch == LF &&
					header_state != h_general_and_quote_and_escape) {
				state = s_header_almost_done;
				CALLBACK_DATA_NOADVANCE(header_value);
				goto reexecute_byte;
			}

			switch (header_state) {
			case h_general:
				if (ch == QT) {
					header_state = h_general_and_quote;
				}

				// fast-forwarding, wheee!
#define MOVE_FAST do {                    \
		++p;                                    \
		ch = *p;                                \
		if (ch == CR || ch == LF || ch == QT) { \
			goto cr_or_lf_or_qt;                  \
		}                                       \
		} while(0);

				if (data + len - p >= 12) {
					MOVE_FAST
					MOVE_FAST
					MOVE_FAST
					MOVE_FAST
					MOVE_FAST
					MOVE_FAST
					MOVE_FAST
					MOVE_FAST
					MOVE_FAST
					MOVE_FAST
					MOVE_FAST
				} else if (data + len - p >= 5) {
					MOVE_FAST
					MOVE_FAST
					MOVE_FAST
					MOVE_FAST
				}

				break;

			case h_general_and_quote:
				if (ch == QT) {
					header_state = h_general;
				} else if (ch == BS) {
					header_state = h_general_and_quote_and_escape;
				}
				break;

			case h_general_and_quote_and_escape:
				header_state = h_general_and_quote;
				break;


			case h_transfer_encoding:
				SET_ERRNO(HPE_INVALID_HEADER_TOKEN);
				goto error;
				break;

			case h_content_length:
				if (ch == ' ') break;
				if (!IS_NUM(ch)) {
					SET_ERRNO(HPE_INVALID_CONTENT_LENGTH);
					goto error;
				}

				if (m_content_length > ((INT64_MAX - 10) / 10)) {
					/* overflow */
					SET_ERRNO(HPE_HUGE_CONTENT_LENGTH);
					goto error;
				}

				m_content_length *= 10;
				m_content_length += ch - '0';
				break;

				/* Transfer-Encoding: chunked */
			case h_matching_transfer_encoding_chunked:
				index++;
				if (index > sizeof(CHUNKED)-1
						|| LOWER(ch) != CHUNKED[index]) {
					header_state = h_general;
				} else if (index == sizeof(CHUNKED)-2) {
					header_state = h_transfer_encoding_chunked;
				}
				break;

			case h_transfer_encoding_chunked:
				if (ch != ' ') header_state = h_general;
				break;

			default:
				state = s_header_value;
				header_state = h_general;
				break;
			}
			break;
		}

		case s_header_almost_done:
		{
			if (ch == LF) {
				state = s_header_value_lws;
			} else {
				state = s_header_value;
			}

			switch (header_state) {
			case h_transfer_encoding_chunked:
				flags |= F_CHUNKED;
				break;
			default:
				break;
			}

			if (ch != LF) {
				CALLBACK_SPACE(header_value);
			}

			break;
		}

		case s_header_value_lws:
		{
			if (ch == ' ' || ch == '\t')
			{
				state = s_header_value_start;
				CALLBACK_SPACE(header_value);
			}
			else
			{
				state = s_header_field_start;
				goto reexecute_byte;
			}
			break;
		}

		case s_headers_almost_done:
		{
			STRICT_CHECK(ch != LF);

			if (flags & F_TRAILING) {
				/* End of a chunked request */
				state = s_message_done;
				CALLBACK_NOTIFY_NOADVANCE(chunk_complete);
				goto reexecute_byte;
			}

			state = s_headers_done;

			/* Set this here so that on_headers_complete() callbacks can see it */
			m_upgrade =
				(flags & F_UPGRADE || m_method == HTTP_CONNECT);

			/* Here we call the headers_complete callback. This is somewhat
			* different than other callbacks because if the user returns 1, we
			* will interpret that as saying that this message has no body. This
			* is needed for the annoying case of receiving a response to a HEAD
			* request.
			*
			* We'd like to use CALLBACK_NOTIFY_NOADVANCE() here but we cannot, so
			* we have to simulate it by handling a change in errno below.
			*/
			size_t header_size = p - data + 1;
			switch (settings.on_headers_complete(*this, nullptr, header_size)) {
			case 0:
				break;

			case 1:
				flags |= F_SKIPBODY;
				break;

			default:
				SET_ERRNO(HPE_CB_headers_complete);
				RETURN(p - data); /* Error */
			}

			if (m_http_errno != HPE_OK) {
				RETURN(p - data);
			}

			goto reexecute_byte;
		}

		case s_headers_done:
		{
			STRICT_CHECK(ch != LF);

			// we're done parsing headers, reset overflow counters
			nread = 0;
			// (if we now move to s_body_*, then this is irrelevant)
			data_or_header_data_start = p;

			int hasBody = flags & F_CHUNKED || m_content_length > 0;
			if (m_upgrade && (m_method == HTTP_CONNECT ||
									(flags & F_SKIPBODY) || !hasBody)) {
				/* Exit, the rest of the message is in a different protocol. */
				state = NEW_MESSAGE();
				CALLBACK_NOTIFY(message_complete);
				RETURN((p - data) + 1);
			}

			if (flags & F_SKIPBODY) {
				state = NEW_MESSAGE();
				CALLBACK_NOTIFY(message_complete);
			} else if (flags & F_CHUNKED) {
				/* chunked encoding - ignore Content-Length header */
				state = s_chunk_size_start;
			} else {
				if (m_content_length == 0) {
					/* Content-Length header given but zero: Content-Length: 0\r\n */
					state = NEW_MESSAGE();
					CALLBACK_NOTIFY(message_complete);
				} else if (m_content_length > 0) {
					/* Content-Length header given and non-zero */
					state = s_body_identity;
				} else {
					unsigned short sc = m_status_code;
					if (type == HTTP_REQUEST ||
							((100 <= sc && sc <= 199) || sc == 204 || sc == 304)) {
						/* Assume content-length 0 - read the next */
						state = NEW_MESSAGE();
						CALLBACK_NOTIFY(message_complete);
					} else {
						/* Read body until EOF */
						state = s_body_identity_eof;
					}
				}
			}

			break;
		}

		case s_body_identity:
		{
			uint64_t to_read = std::min(m_content_length, (data + len) - p);

			assert(m_content_length > 0);

			/* The difference between advancing content_length and p is because
			* the latter will automatically advance on the next loop iteration.
			* Further, if content_length ends up at 0, we want to see the last
			* byte again for our message complete callback.
			*/
			MARK(body);
			m_content_length -= to_read;
			p += to_read - 1;

			if (m_content_length == 0) {
				state = s_message_done;

				/* Mimic CALLBACK_DATA_NOADVANCE() but with one extra byte.
				*
				* The alternative to doing this is to wait for the next byte to
				* trigger the data callback, just as in every other case. The
				* problem with this is that this makes it difficult for the test
				* harness to distinguish between complete-on-EOF and
				* complete-on-length. It's not clear that this distinction is
				* important for applications, but let's keep it for now.
				*/
				_CALLBACK_DATA(body, p - body_mark + 1, p - data);
				goto reexecute_byte;
			}

			break;
		}

		/* read until EOF */
		case s_body_identity_eof:
			MARK(body);
			p = data + len - 1;

			break;

		case s_message_done:
			state = NEW_MESSAGE();
			nread = 0;
			data_or_header_data_start = p;
			CALLBACK_NOTIFY(message_complete);
			if (m_upgrade) {
				/* Exit, the rest of the message is in a different protocol. */
				RETURN((p - data) + 1);
			}
			break;

		case s_chunk_size_start:
		{
			assert(flags & F_CHUNKED);

			unhex_val = unhex[(unsigned char)ch];
			if (unhex_val == -1) {
				SET_ERRNO(HPE_INVALID_CHUNK_SIZE);
				goto error;
			}

			m_content_length = unhex_val;
			state = s_chunk_size;
			break;
		}

		case s_chunk_size:
		{
			assert(flags & F_CHUNKED);

			if (ch == CR) {
				state = s_chunk_size_almost_done;
				break;
			}

			unhex_val = unhex[(unsigned char)ch];

			if (unhex_val == -1) {
				if (ch == ';' || ch == ' ') {
					state = s_chunk_parameters;
					break;
				}

				SET_ERRNO(HPE_INVALID_CHUNK_SIZE);
				goto error;
			}

			if (m_content_length > (INT64_MAX - unhex_val) >> 4) {
				/* overflow */
				SET_ERRNO(HPE_HUGE_CHUNK_SIZE);
				goto error;
			}
			m_content_length *= 16;
			m_content_length += unhex_val;
			break;
		}

		case s_chunk_parameters:
		{
			assert(flags & F_CHUNKED);
			/*
			* just ignore this shit. TODO check for overflow
			* TODO: It would be nice to pass this information to the
			* on_chunk_header callback.
			*/
			if (ch == CR) {
				state = s_chunk_size_almost_done;
				break;
			}
			break;
		}

		case s_chunk_size_almost_done:
		{
			assert(flags & F_CHUNKED);
			STRICT_CHECK(ch != LF);

			if (m_content_length == 0) {
				flags |= F_TRAILING;
				state = s_header_field_start;
				CALLBACK_NOTIFY(chunk_header);
			} else {
				state = s_chunk_data;
				CALLBACK_NOTIFY(chunk_header);
			}
			break;
		}

		case s_chunk_data:
		{
			uint64_t to_read = std::min(m_content_length, (data + len) - p);

			assert(flags & F_CHUNKED);
			assert(m_content_length > 0);

			/* See the explanation in s_body_identity for why the content
			* length and data pointers are managed this way.
			*/
			MARK(body);
			m_content_length -= to_read;
			p += to_read - 1;

			if (m_content_length == 0) {
				state = s_chunk_data_almost_done;
			}

			break;
		}

		case s_chunk_data_almost_done:
			assert(flags & F_CHUNKED);
			assert(m_content_length == 0);
			STRICT_CHECK(ch != CR);
			state = s_chunk_data_done;
			CALLBACK_DATA(body);
			break;

		case s_chunk_data_done:
			assert(flags & F_CHUNKED);
			STRICT_CHECK(ch != LF);
			state = s_chunk_size_start;
			nread = 0;
			data_or_header_data_start = p;
			CALLBACK_NOTIFY(chunk_complete);
			break;

		default:
			assert(0 && "unhandled state");
			SET_ERRNO(HPE_INVALID_INTERNAL_STATE);
			goto error;
		}
	}

	/* We can check for overflow here because in Proxygen, len <= ~8KB and so the
	* worst thing that can happen is that we catch the overflow at 88KB rather
	* than at 80KB.
	* In case of chunk encoding, we count the overflow for every
	* chunk separately.
	* We zero the nread counter (and reset data_or_header_data_start) when we
	* start parsing a new message or a new chunk.
	*/
	if (PARSING_HEADER(state)) {
		nread += p - data_or_header_data_start;
		if (nread > HTTP_MAX_HEADER_SIZE) {
			SET_ERRNO(HPE_HEADER_OVERFLOW);
			goto error;
		}
	}

	/* Run callbacks for any marks that we have leftover after we ran out of
	* bytes. There should be at most one of these set, so it's OK to invoke
	* them in series (unset marks will not result in callbacks).
	*
	* We use the NOADVANCE() variety of callbacks here because 'p' has already
	* overflowed 'data' and this allows us to correct for the off-by-one that
	* we'd otherwise have (since CALLBACK_DATA() is meant to be run with a 'p'
	* value that's in-bounds).
	*/

	assert(((header_field_mark ? 1 : 0) +
			(header_value_mark ? 1 : 0) +
			(url_mark ? 1 : 0)  +
			(reason_mark ? 1 : 0)  +
			(body_mark ? 1 : 0)) <= 1);

	CALLBACK_DATA_NOADVANCE(header_field);
	CALLBACK_DATA_NOADVANCE(header_value);
	CALLBACK_DATA_NOADVANCE(url);
	CALLBACK_DATA_NOADVANCE(reason);
	CALLBACK_DATA_NOADVANCE(body);

	RETURN(len);

error:
	if (m_http_errno == HPE_OK) {
		SET_ERRNO(HPE_UNKNOWN);
	}

	RETURN(p - data);
}

void http_parser::pause(int paused)
{
    /* Users should only be pausing/unpausing a parser that is not in an error
     * state. In non-debug builds, there's not much that we can do about this
     * other than ignore it.
     */

    if (m_http_errno == HPE_OK || m_http_errno == HPE_PAUSED) {
        SET_ERRNO((paused) ? HPE_PAUSED : HPE_OK);
    } else {
        assert(0 && "Attempting to pause parser in error state");
    }
}

const char * http_parser::method_str (enum http_method m)
{
  return method_strings[m];
}

const char* http_parser::http_errno::name()
{
  assert(m_errno < (sizeof(http_strerror_tab)/sizeof(http_strerror_tab[0])));
  return http_strerror_tab[m_errno].name;
}

const char* http_parser::http_errno::description()
{
  assert(m_errno < (sizeof(http_strerror_tab)/sizeof(http_strerror_tab[0])));
  return http_strerror_tab[m_errno].description;
}
