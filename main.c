/*
 * Copyright (C) 2016  Alex Yatskov <alex@foosoft.net>
 * Author: Alex Yatskov <alex@foosoft.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#include "convert.h"
#include "util.h"
#include "book.h"
#include "hooks.h"
#include "gaiji.h"

#include "eb/eb/eb.h"
#include "eb/text.h"
#include "eb/eb/error.h"

/*
 * Local functions
 */

static void export_subbook_entries(Book_Subbook* subbook, EB_Book* eb_book, EB_Hookset* eb_hookset, Gaiji_Context* context) {
    if (subbook->entry_capacity == 0) {
        subbook->entry_capacity = 16384;
        subbook->entries = malloc(subbook->entry_capacity * sizeof(Book_Entry));
    }

    EB_Hit hits[256];
    int hit_count = 0;

    do {
        if (eb_hit_list(eb_book, ARRSIZE(hits), hits, &hit_count) != EB_SUCCESS) {
            continue;
        }

        for (int i = 0; i < hit_count; ++i) {
            EB_Hit* hit = hits + i;

            if (subbook->entry_count == subbook->entry_capacity) {
                subbook->entry_capacity *= 2;
                subbook->entries = realloc(subbook->entries, subbook->entry_capacity * sizeof(Book_Entry));
            }

            Book_Entry* entry = subbook->entries + subbook->entry_count++;
            entry->heading = book_read(eb_book, eb_hookset, &hit->heading, BOOK_MODE_HEADING, context);
            entry->text = book_read(eb_book, eb_hookset, &hit->text, BOOK_MODE_TEXT, context);
        }
    }
    while (hit_count > 0);
}

static void export_subbook(Book_Subbook* subbook, EB_Book* eb_book, EB_Hookset* eb_hookset) {
    Gaiji_Context context = {};
    char title[EB_MAX_TITLE_LENGTH + 1];
    if (eb_subbook_title(eb_book, title) == EB_SUCCESS) {
        subbook->title = eucjp_to_utf8(title);
        context = *gaiji_context_select(subbook->title);
    }

    if (eb_have_copyright(eb_book)) {
        EB_Position position;
        if (eb_copyright(eb_book, &position) == EB_SUCCESS) {
            subbook->copyright = book_read(eb_book, eb_hookset, &position, BOOK_MODE_TEXT, &context);
        }
    }

    if (eb_search_all_alphabet(eb_book) == EB_SUCCESS) {
        export_subbook_entries(subbook, eb_book, eb_hookset, &context);
    }

    if (eb_search_all_kana(eb_book) == EB_SUCCESS) {
        export_subbook_entries(subbook, eb_book, eb_hookset, &context);
    }

    if (eb_search_all_asis(eb_book) == EB_SUCCESS) {
        export_subbook_entries(subbook, eb_book, eb_hookset, &context);
    }
}

static void export_book(Book* book, const char path[]) {
    do {
        EB_Error_Code error;
        if ((error = eb_initialize_library()) != EB_SUCCESS) {
            fprintf(stderr, "Failed to initialize library: %s\n", eb_error_message(error));
            break;
        }

        EB_Book eb_book;
        eb_initialize_book(&eb_book);

        EB_Hookset eb_hookset;
        eb_initialize_hookset(&eb_hookset);
        hooks_install(&eb_hookset);

        if ((error = eb_bind(&eb_book, path)) != EB_SUCCESS) {
            fprintf(stderr, "Failed to bind book: %s\n", eb_error_message(error));
            eb_finalize_book(&eb_book);
            eb_finalize_hookset(&eb_hookset);
            eb_finalize_library();
            break;
        }

        EB_Character_Code character_code;
        if (eb_character_code(&eb_book, &character_code) == EB_SUCCESS) {
            switch (character_code) {
                case EB_CHARCODE_ISO8859_1:
                    strcpy(book->character_code, "iso8859-1");
                    break;
                case EB_CHARCODE_JISX0208:
                    strcpy(book->character_code, "jisx0208");
                    break;
                case EB_CHARCODE_JISX0208_GB2312:
                    strcpy(book->character_code, "jisx0208/gb2312");
                    break;
                default:
                    strcpy(book->character_code, "invalid");
                    break;
            }
        }

        EB_Disc_Code disc_code;
        if (eb_disc_type(&eb_book, &disc_code) == EB_SUCCESS) {
            switch (disc_code) {
                case EB_DISC_EB:
                    strcpy(book->disc_code, "eb");
                    break;
                case EB_DISC_EPWING:
                    strcpy(book->disc_code, "epwing");
                    break;
                default:
                    strcpy(book->disc_code, "invalid");
                    break;
            }
        }

        EB_Subbook_Code sub_codes[EB_MAX_SUBBOOKS];
        if ((error = eb_subbook_list(&eb_book, sub_codes, &book->subbook_count)) == EB_SUCCESS) {
            if (book->subbook_count > 0) {
                book->subbooks = calloc(book->subbook_count, sizeof(Book_Subbook));
                for (int i = 0; i < book->subbook_count; ++i) {
                    Book_Subbook* subbook = book->subbooks + i;
                    if ((error = eb_set_subbook(&eb_book, sub_codes[i])) == EB_SUCCESS) {
                        export_subbook(subbook, &eb_book, &eb_hookset);
                    }
                    else {
                        fprintf(stderr, "Failed to set subbook: %s\n", eb_error_message(error));
                    }
                }
            }
        }
        else {
            fprintf(stderr, "Failed to get subbook list: %s\n", eb_error_message(error));
        }

        eb_finalize_book(&eb_book);
        eb_finalize_hookset(&eb_hookset);
        eb_finalize_library();
    }
    while(0);
}

/*
 * Entry point
 */

int main(int argc, char *argv[]) {
    bool pretty_print = false;

    char opt = 0;
    while ((opt = getopt(argc, argv, "p")) != -1) {
        switch (opt) {
            case 'p':
                pretty_print = true;
                break;
            default:
                exit(EXIT_FAILURE);
                break;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Usage: %s [-p] dictionary_path\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    Book book = {};
    export_book(&book, argv[optind]);
    book_dump(&book, pretty_print, stdout);
    book_free(&book);

    return 0;
}
