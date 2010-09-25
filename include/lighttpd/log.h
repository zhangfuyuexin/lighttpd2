#ifndef _LIGHTTPD_LOG_H_
#define _LIGHTTPD_LOG_H_

#ifndef _LIGHTTPD_BASE_H_
#error Please include <lighttpd/base.h> instead of this file
#endif

/*
 * Logging uses a dedicated thread in order to prevent blocking write io from blocking normal operations in worker threads.
 * Code handling vrequests should use the VR_ERROR(), VR_DEBUG() etc makros. Otherwise the ERROR(), DEBUG() etc makros should be used.
 * Basic examples: VR_WARNING(vr, "%s", "something unexpected happened")   ERROR(srv, "%d is not bigger than %d", 23, 42)
 *
 * Log targets specify where the log messages are written to. They are kept open for a certain amount of time (default 30s).
 * file://
 *
 * Logs are sent once per ev_loop() iteration to the logging thread in order to reduce syscalls and lock contention.
 */

/* #include <lighttpd/valgrind/valgrind.h> */

#define _SEGFAULT(srv, vr, fmt, ...) \
	do { \
		li_log_write(srv, NULL, LI_LOG_LEVEL_ABORT, LOG_FLAG_TIMESTAMP, "(crashing) %s.%d: "fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__); \
		/* VALGRIND_PRINTF_BACKTRACE(fmt, __VA_ARGS__); */\
		abort();\
	} while(0)

#define _ERROR(srv, vr, fmt, ...) \
	li_log_write(srv, vr, LI_LOG_LEVEL_ERROR, LOG_FLAG_TIMESTAMP, "(error) %s.%d: "fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define _WARNING(srv, vr, fmt, ...) \
	li_log_write(srv, vr, LI_LOG_LEVEL_WARNING, LOG_FLAG_TIMESTAMP, "(warning) %s.%d: "fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define _INFO(srv, vr, fmt, ...) \
	li_log_write(srv, vr, LI_LOG_LEVEL_INFO, LOG_FLAG_TIMESTAMP, "(info) %s.%d: "fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define _DEBUG(srv, vr, fmt, ...) \
	li_log_write(srv, vr, LI_LOG_LEVEL_DEBUG, LOG_FLAG_TIMESTAMP, "(debug) %s.%d: "fmt, LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__)

#define _BACKEND(srv, vr, fmt, ...) \
	li_log_write(srv, vr, LI_LOG_LEVEL_BACKEND, LOG_FLAG_TIMESTAMP, fmt, __VA_ARGS__)
#define _BACKEND_LINES(srv, vr, txt, fmt, ...) \
	li_log_split_lines_(srv, vr, LI_LOG_LEVEL_BACKEND, LOG_FLAG_TIMESTAMP, txt, fmt, __VA_ARGS__)

#define _GERROR(srv, vr, error, fmt, ...) \
	li_log_write(srv, vr, LI_LOG_LEVEL_ERROR, LOG_FLAG_TIMESTAMP, "(error) %s.%d: " fmt "\n  %s", LI_REMOVE_PATH(__FILE__), __LINE__, __VA_ARGS__, error ? error->message : "Empty GError")

#define VR_SEGFAULT(vr, fmt, ...) _SEGFAULT(vr->wrk->srv, vr, fmt, __VA_ARGS__)
#define VR_ERROR(vr, fmt, ...)    _ERROR(vr->wrk->srv, vr, fmt, __VA_ARGS__)
#define VR_WARNING(vr, fmt, ...)  _WARNING(vr->wrk->srv, vr, fmt, __VA_ARGS__)
#define VR_INFO(vr, fmt, ...)     _INFO(vr->wrk->srv, vr, fmt, __VA_ARGS__)
#define VR_DEBUG(vr, fmt, ...)    _DEBUG(vr->wrk->srv, vr, fmt, __VA_ARGS__)
#define VR_BACKEND(vr, fmt, ...)  _BACKEND(vr->wrk->srv, vr, fmt, __VA_ARGS__)
#define VR_BACKEND_LINES(vr, txt, fmt, ...) _BACKEND_LINES(vr->wrk->srv, vr, txt, fmt, __VA_ARGS__)
#define VR_GERROR(vr, error, fmt, ...) _GERROR(vr->wrk->srv, vr, error, fmt, __VA_ARGS__)

#define SEGFAULT(srv, fmt, ...)   _SEGFAULT(srv, NULL, fmt, __VA_ARGS__)
#define ERROR(srv, fmt, ...)      _ERROR(srv, NULL, fmt, __VA_ARGS__)
#define WARNING(srv, fmt, ...)    _WARNING(srv, NULL, fmt, __VA_ARGS__)
#define INFO(srv, fmt, ...)       _INFO(srv, NULL, fmt, __VA_ARGS__)
#define DEBUG(srv, fmt, ...)      _DEBUG(srv, NULL, fmt, __VA_ARGS__)
#define BACKEND(srv, fmt, ...)    _BACKEND(srv, NULL, fmt, __VA_ARGS__)
#define GERROR(srv, error, fmt, ...) _GERROR(srv, NULL, error, fmt, __VA_ARGS__)

/* flags for li_log_write */
#define LOG_FLAG_NONE         (0x0)      /* default flag */
#define LOG_FLAG_TIMESTAMP    (0x1)      /* prepend a timestamp to the log message */
#define LOG_FLAG_NOLOCK       (0x1 << 1) /* for internal use only */

struct liLog {
	liLogType type;
	GString *path;
	gint fd;
	liWaitQueueElem wqelem;
};

struct liLogTimestamp {
	gint refcount;
	ev_tstamp last_ts;
	GString *format;
	GString *cached;
};

struct liLogEntry {
	GString *path;
	liLogTimestamp *ts;
	liLogLevel level;
	guint flags;
	GString *msg;
	GList queue_link;
};

/* determines the type of a log target by the path given. /absolute/path = file; |app = pipe; stderr = stderr; syslog = syslog;
 * returns the begin of the parameter string in *param if param != NULL (filename for /absolute/path or file:///absolute/path)
 *   *param is either NULL or points into the path string!
 */
LI_API liLogType li_log_type_from_path(GString *path, gchar **param);

LI_API liLogLevel li_log_level_from_string(GString *str);
LI_API gchar* li_log_level_str(liLogLevel log_level);

/* log_new is used to create a new log target, if a log with the same path already exists, it is referenced instead */
LI_API liLog *li_log_new(liServer *srv, liLogType type, GString *path);

LI_API void li_log_thread_start(liServer *srv);
LI_API void li_log_thread_wakeup(liServer *srv);
LI_API void li_log_thread_stop(liServer *srv);
LI_API void li_log_thread_finish(liServer *srv);

LI_API void li_log_init(liServer *srv);
LI_API void li_log_cleanup(liServer *srv);

LI_API gboolean li_log_write_direct(liServer *srv, liVRequest *vr, GString *path, GString *msg);
/* li_log_write is used to write to the errorlog */
LI_API gboolean li_log_write(liServer *srv, liVRequest *vr, liLogLevel log_level, guint flags, const gchar *fmt, ...) G_GNUC_PRINTF(5, 6);

LI_API liLogTimestamp *li_log_timestamp_new(liServer *srv, GString *format);
LI_API gboolean li_log_timestamp_free(liServer *srv, liLogTimestamp *ts);

/* replaces '\r' and '\n' with '\0' */
LI_API void li_log_split_lines(liServer *srv, liVRequest *vr, liLogLevel log_level, guint flags, gchar *txt, const gchar *prefix);
LI_API void li_log_split_lines_(liServer *srv, liVRequest *vr, liLogLevel log_level, guint flags, gchar *txt, const gchar *fmt, ...) G_GNUC_PRINTF(6, 7);


#endif
