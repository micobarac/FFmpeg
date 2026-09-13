/*
 * Copyright (c) 2010  Aurelien Jacobs <aurel@gnuage.org>
 * Copyright (c) 2017  Clément Bœsch <u@pkh.me>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/parseutils.h"
#include "htmlsubtitles.h"
#include "htmlfont.h"
#include <ctype.h>

static void rstrip_spaces_buf(AVBPrint *buf)
{
    if (av_bprint_is_complete(buf))
        while (buf->len > 0 && buf->str[buf->len - 1] == ' ')
            buf->str[--buf->len] = 0;
}

/*
 * Fast code for scanning text enclosed in braces. Functionally
 * equivalent to this sscanf call:
 *
 * sscanf(in, "{\\an%*1u}%n", &len) >= 0 && len > 0
 */
static int scanbraces(const char* in) {
    if (strncmp(in, "{\\an", 4) != 0) {
        return 0;
    }
    if (!av_isdigit(in[4])) {
        return 0;
    }
    if (in[5] != '}') {
        return 0;
    }
    return 1;
}

/* skip all {\xxx} substrings except for {\an%d}
   and all microdvd like styles such as {Y:xxx} */
static void handle_open_brace(AVBPrint *dst, const char **inp, int *an, int *closing_brace_missing)
{
    const char *in = *inp;

    *an += scanbraces(in);

    if (!*closing_brace_missing) {
        if (   (*an != 1 && in[1] == '\\')
            || (in[1] && strchr("CcFfoPSsYy", in[1]) && in[2] == ':')) {
            char *bracep = strchr(in+2, '}');
            if (bracep) {
                *inp = bracep;
                return;
            } else
                *closing_brace_missing = 1;
        }
    }

    av_bprint_chars(dst, *in, 1);
}

/*
 * Fast code for scanning the rest of a tag. Functionally equivalent to
 * this sscanf call:
 *
 * sscanf(in, "%127[^<>]>%n", buffer, lenp) == 2
 */
static int scantag(const char* in, char* buffer, int* lenp) {
    int len;

    for (len = 0; len < 128; len++) {
        const char c = *in++;
        switch (c) {
        case '\0':
            return 0;
        case '<':
            return 0;
        case '>':
            buffer[len] = '\0';
            *lenp = len+1;
            return 1;
        default:
            break;
        }
        buffer[len] = c;
    }
    return 0;
}

/*
 * The general politic of the convert is to mask unsupported tags or formatting
 * errors (but still alert the user/subtitles writer with an error/warning)
 * without dropping any actual text content for the final user.
 */
int ff_htmlmarkup_to_ass(void *log_ctx, AVBPrint *dst, const char *in)
{
    char *param, buffer[128];
    int len, tag_close, line_start = 1, an = 0, end = 0;
    int closing_brace_missing = 0;
    int i, likely_a_tag;

    struct torro_fonts fonts = {0};
    const char *input_end = in + strlen(in);

    for (; !end && *in; in++) {
        switch (*in) {
        case '\r':
            break;
        case '\n':
            if (line_start) {
                end = 1;
                break;
            }
            rstrip_spaces_buf(dst);
            av_bprintf(dst, "\\N");
            line_start = 1;
            break;
        case ' ':
            if (!line_start)
                av_bprint_chars(dst, *in, 1);
            break;
        case '{':
            handle_open_brace(dst, &in, &an, &closing_brace_missing);
            break;
        case '<': {
            /*
             * "<<" are likely latin guillemets in ASCII or some kind of random
             * style effect; see sub/badsyntax.srt in the FATE samples
             * directory for real test cases.
             */

            likely_a_tag = 1;
            for (i = 0; in[1] == '<'; i++) {
                av_bprint_chars(dst, '<', 1);
                likely_a_tag = 0;
                in++;
            }

            tag_close = in[1] == '/';
            if (tag_close)
                likely_a_tag = 1;

            av_assert0(in[0] == '<');

            size_t font_len = torro_font_tag(dst, in, input_end - in, &fonts, log_ctx);
            if (font_len) {
                in += font_len - 1;
                break;
            }

            len = 0;

            if (scantag(in+tag_close+1, buffer, &len) && len > 0) {
                const int skip = len + tag_close;
                const char *tagname = buffer;
                while (*tagname == ' ') {
                    likely_a_tag = 0;
                    tagname++;
                }
                if ((param = strchr(tagname, ' ')))
                    *param++ = 0;

                /* Check if this is likely a tag */
#define LIKELY_A_TAG_CHAR(x) (((x) >= '0' && (x) <= '9') || \
                              ((x) >= 'a' && (x) <= 'z') || \
                              ((x) >= 'A' && (x) <= 'Z') || \
                               (x) == '_' || (x) == '/')
                for (i = 0; tagname[i]; i++) {
                    if (!LIKELY_A_TAG_CHAR(tagname[i])) {
                        likely_a_tag = 0;
                        break;
                    }
                }

                if (tagname[0] && !tagname[1] && strchr("bisu", av_tolower(tagname[0]))) {
                    av_bprintf(dst, "{\\%c%d}", (char)av_tolower(tagname[0]), !tag_close);
                    in += skip;
                } else if (!av_strncasecmp(tagname, "br", 2) &&
                           (!tagname[2] || (tagname[2] == '/' && !tagname[3]))) {
                    av_bprintf(dst, "\\N");
                    in += skip;
                } else if (likely_a_tag) {
                    if (!tag_close) // warn only once
                        av_log(log_ctx, AV_LOG_WARNING, "Unrecognized tag %s\n", tagname);
                    in += skip;
                } else {
                    av_bprint_chars(dst, '<', 1);
                }
            } else {
                av_bprint_chars(dst, *in, 1);
            }
            break;
        }
        default:
            av_bprint_chars(dst, *in, 1);
            break;
        }
        if (*in != ' ' && *in != '\r' && *in != '\n')
            line_start = 0;
    }

    if (!av_bprint_is_complete(dst))
        return AVERROR(ENOMEM);

    while (dst->len >= 2 && !strncmp(&dst->str[dst->len - 2], "\\N", 2))
        dst->len -= 2;
    dst->str[dst->len] = 0;
    rstrip_spaces_buf(dst);

    return 0;
}
