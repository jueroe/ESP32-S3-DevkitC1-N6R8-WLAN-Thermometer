/*
 * Small helpers for text that goes into HTML or JSON.
 *
 * Both escape functions always terminate the target, so the result can be sent
 * directly.  When the target is too small, the text is cut instead of
 * overflowing.
 */

#pragma once

#include <stddef.h>

/** Replace & < > " ' by their HTML entities. */
void html_escape(const char *source, char *target, size_t size);

/** Escape " and \\ and drop control characters, so the result fits into a JSON string. */
void json_escape(const char *source, char *target, size_t size);
