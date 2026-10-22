#include "textutil.h"

#include <string.h>

void html_escape(const char *source, char *target, size_t size)
{
    if (target == NULL || size == 0) {
        return;
    }

    size_t out = 0;
    for (size_t i = 0; source[i] != '\0' && out + 7 < size; i++) {
        const char *replacement = NULL;
        switch (source[i]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default: break;
        }

        if (replacement != NULL) {
            const size_t length = strlen(replacement);
            memcpy(&target[out], replacement, length);
            out += length;
        } else {
            target[out++] = source[i];
        }
    }
    target[out] = '\0';
}

void json_escape(const char *source, char *target, size_t size)
{
    if (target == NULL || size == 0) {
        return;
    }

    size_t out = 0;
    for (size_t i = 0; source[i] != '\0' && out + 2 < size; i++) {
        const unsigned char byte = (unsigned char)source[i];
        if (byte == '"' || byte == '\\') {
            target[out++] = '\\';
            target[out++] = (char)byte;
        } else if (byte < 0x20) {
            continue; /* control characters are not allowed in a JSON string */
        } else {
            target[out++] = (char)byte;
        }
    }
    target[out] = '\0';
}
