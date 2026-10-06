/*
 * Error reporting
 *
 * Copyright (C) 2010 Red Hat Inc.
 *
 * Authors:
 *  Markus Armbruster <armbru@redhat.com>,
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "monitor/monitor.h"
#include "qemu/error-report.h"

/*
 * @report_type is the type of message: error, warning or
 * informational.
 */
typedef enum {
    REPORT_TYPE_ERROR,
    REPORT_TYPE_WARNING,
    REPORT_TYPE_INFO,
} report_type;

void error_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    error_vprintf(fmt, ap);
    va_end(ap);
}

void error_printf_unless_qmp(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    error_vprintf_unless_qmp(fmt, ap);
    va_end(ap);
}

static Location std_loc = {
    .kind = LOC_NONE
};
static Location *cur_loc = &std_loc;

/*
 * Push location saved in LOC onto the location stack, return it.
 * The top of that stack is the current location.
 * Needs a matching loc_pop().
 */
Location *loc_push_restore(Location *loc)
{
    assert(!loc->prev);
    loc->prev = cur_loc;
    cur_loc = loc;
    return loc;
}

/*
 * Initialize *LOC to "nowhere", push it onto the location stack.
 * The top of that stack is the current location.
 * Needs a matching loc_pop().
 * Return LOC.
 */
Location *loc_push_none(Location *loc)
{
    loc->kind = LOC_NONE;
    loc->prev = NULL;
    return loc_push_restore(loc);
}

/*
 * Pop the location stack.
 * LOC must be the current location, i.e. the top of the stack.
 */
Location *loc_pop(Location *loc)
{
    assert(cur_loc == loc && loc->prev);
    cur_loc = loc->prev;
    loc->prev = NULL;
    return loc;
}

/*
 * Save the current location in LOC, return LOC.
 */
Location *loc_save(Location *loc)
{
    *loc = *cur_loc;
    loc->prev = NULL;
    return loc;
}

/*
 * Change the current location to the one saved in LOC.
 */
void loc_restore(Location *loc)
{
    Location *prev = cur_loc->prev;
    assert(!loc->prev);
    *cur_loc = *loc;
    cur_loc->prev = prev;
}

/*
 * Change the current location to "nowhere in particular".
 */
void loc_set_none(void)
{
    cur_loc->kind = LOC_NONE;
}

/*
 * Change the current location to argument ARGV[IDX..IDX+CNT-1].
 */
void loc_set_cmdline(const char **argv, int idx, int cnt)
{
    cur_loc->kind = LOC_CMDLINE;
    cur_loc->num = cnt;
    cur_loc->ptr = argv + idx;
}

/*
 * Change the current location to file FNAME, line LNO.
 */
void loc_set_file(const char *fname, int lno)
{
    assert (fname || cur_loc->kind == LOC_FILE);
    cur_loc->kind = LOC_FILE;
    cur_loc->num = lno;
    if (fname) {
        cur_loc->ptr = fname;
    }
}

static const char *progname;

/*
 * Set the program name for error_print_loc().
 */
void error_set_progname(const char *argv0)
{
    const char *p = strrchr(argv0, '/');
    progname = p ? p + 1 : argv0;
}

const char *error_get_progname(void)
{
    return progname;
}

/*
 * Append the current location to @msg.
 */
static void print_loc(GString *msg)
{
    const char *sep = "";
    int i;
    const char *const *argp;

    if (!cur_mon && progname) {
        g_string_append_printf(msg, "%s:", progname);
        sep = " ";
    }
    switch (cur_loc->kind) {
    case LOC_CMDLINE:
        argp = cur_loc->ptr;
        for (i = 0; i < cur_loc->num; i++) {
            g_string_append_printf(msg, "%s%s", sep, argp[i]);
            sep = " ";
        }
        g_string_append(msg, ": ");
        break;
    case LOC_FILE:
        g_string_append_printf(msg, "%s:", (const char *)cur_loc->ptr);
        if (cur_loc->num) {
            g_string_append_printf(msg, "%d:", cur_loc->num);
        }
        g_string_append(msg, " ");
        break;
    default:
        g_string_append(msg, sep);
    }
}

bool enable_timestamp_msg;
/*
 * Print a message to current monitor if we have one, else to stderr.
 * @report_type is the type of message: error, warning or informational.
 * Format arguments like vsprintf().  The resulting message should be
 * a single phrase, with no newline or trailing punctuation.
 * Prepend the current location and append a newline.
 * It's wrong to call this in a QMP monitor.  Use error_setg() there.
 */
static void vreport(report_type type, const char *fmt, va_list ap)
{
    GTimeVal tv;
    gchar *timestr;
    GString *msg = g_string_new(NULL);

    if (enable_timestamp_msg && !cur_mon) {
        g_get_current_time(&tv);
        timestr = g_time_val_to_iso8601(&tv);
        g_string_append_printf(msg, "%s ", timestr);
        g_free(timestr);
    }

    print_loc(msg);

    switch (type) {
    case REPORT_TYPE_ERROR:
        break;
    case REPORT_TYPE_WARNING:
        g_string_append(msg, "warning: ");
        break;
    case REPORT_TYPE_INFO:
        g_string_append(msg, "info: ");
        break;
    }

    g_string_append_vprintf(msg, fmt, ap);
    g_string_append(msg, "\n");

    /*
     * Hand the whole line over in one call. Not every host can line
     * buffer stderr (the Windows CRT cannot), and a line printed piece by
     * piece may reach the reader in several writes, or interleaved with
     * output from other threads.
     */
    error_printf("%s", msg->str);
    g_string_free(msg, true);
}

/*
 * Print an error message to current monitor if we have one, else to stderr.
 * Format arguments like vsprintf().  The resulting message should be
 * a single phrase, with no newline or trailing punctuation.
 * Prepend the current location and append a newline.
 * It's wrong to call this in a QMP monitor.  Use error_setg() there.
 */
void error_vreport(const char *fmt, va_list ap)
{
    vreport(REPORT_TYPE_ERROR, fmt, ap);
}

/*
 * Print a warning message to current monitor if we have one, else to stderr.
 * Format arguments like vsprintf().  The resulting message should be
 * a single phrase, with no newline or trailing punctuation.
 * Prepend the current location and append a newline.
 * It's wrong to call this in a QMP monitor.  Use error_setg() there.
 */
void warn_vreport(const char *fmt, va_list ap)
{
    vreport(REPORT_TYPE_WARNING, fmt, ap);
}

/*
 * Print an information message to current monitor if we have one, else to
 * stderr.
 * Format arguments like vsprintf().  The resulting message should be
 * a single phrase, with no newline or trailing punctuation.
 * Prepend the current location and append a newline.
 * It's wrong to call this in a QMP monitor.  Use error_setg() there.
 */
void info_vreport(const char *fmt, va_list ap)
{
    vreport(REPORT_TYPE_INFO, fmt, ap);
}

/*
 * Print an error message to current monitor if we have one, else to stderr.
 * Format arguments like sprintf().  The resulting message should be
 * a single phrase, with no newline or trailing punctuation.
 * Prepend the current location and append a newline.
 * It's wrong to call this in a QMP monitor.  Use error_setg() there.
 */
void error_report(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vreport(REPORT_TYPE_ERROR, fmt, ap);
    va_end(ap);
}

/*
 * Print a warning message to current monitor if we have one, else to stderr.
 * Format arguments like sprintf(). The resulting message should be a
 * single phrase, with no newline or trailing punctuation.
 * Prepend the current location and append a newline.
 * It's wrong to call this in a QMP monitor.  Use error_setg() there.
 */
void warn_report(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vreport(REPORT_TYPE_WARNING, fmt, ap);
    va_end(ap);
}

/*
 * Print an information message to current monitor if we have one, else to
 * stderr.
 * Format arguments like sprintf(). The resulting message should be a
 * single phrase, with no newline or trailing punctuation.
 * Prepend the current location and append a newline.
 * It's wrong to call this in a QMP monitor.  Use error_setg() there.
 */
void info_report(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vreport(REPORT_TYPE_INFO, fmt, ap);
    va_end(ap);
}
