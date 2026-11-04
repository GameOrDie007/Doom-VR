/* The glib pieces this FluidSynth uses, on POSIX threads - for Android (Doom VR's
 * standalone build), where there is no glib. The same set the Win32 stubs
 * (win32_glibstubs.h) provide, with pthreads, clock_gettime and GCC's atomics in
 * place of the Win32 calls. */
#ifndef _POSIX_GLIBSTUBS_H
#define _POSIX_GLIBSTUBS_H

#if defined(__ANDROID__) || defined(__linux__)
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <alloca.h>
#include <pthread.h>
#include <unistd.h>

#ifndef BOOL
typedef int BOOL;
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* Miscellaneous stubs */
#define GLIB_CHECK_VERSION(x, y, z) 0 /* Evaluate to 0 to get FluidSynth to use the "old" thread API */
#define GLIB_MAJOR_VERSION 2
#define GLIB_MINOR_VERSION 29

typedef struct
{
    int code;
    const char *message;
} GError;
typedef void *gpointer;

#define g_new(s, c) FLUID_ARRAY(s, c)
#define g_free(p) FLUID_FREE(p)
#define g_strfreev FLUID_FREE
#define g_newa(_type, _len) (_type *)alloca(sizeof(_type) * (_len))
#define g_assert(a) assert(a)
#define G_LIKELY(expr) (expr)
#define G_UNLIKELY(expr) (expr)

#define g_vsnprintf(b, c, f, a) vsnprintf(b, c, f, a)
#define g_snprintf(b, c, f, ...) snprintf(b, c, f, __VA_ARGS__)

#define g_return_val_if_fail(expr, val) if (expr) {} else { return val; }
#define g_clear_error(err) do {} while (0)

#define G_FILE_TEST_EXISTS 1
#define G_FILE_TEST_IS_REGULAR 2

#define g_file_test fluid_g_file_test
#define g_shell_parse_argv fluid_g_shell_parse_argv
BOOL fluid_g_file_test(const char *path, int flags);
BOOL fluid_g_shell_parse_argv(const char *command_line, int *argcp, char ***argvp, void *dummy);

#define g_get_monotonic_time fluid_g_get_monotonic_time
double fluid_g_get_monotonic_time(void);

/* glib/gstdio.h's file calls, and the old GTimeVal clock */
#include <sys/stat.h>
#include <sys/time.h>
#define g_stat(_p, _b) stat((_p), (_b))
#define g_fopen(_p, _m) fopen((_p), (_m))
typedef struct { long tv_sec; long tv_usec; } GTimeVal;
static inline void g_get_current_time(GTimeVal *tv) { struct timeval t; gettimeofday(&t, NULL); tv->tv_sec = (long)t.tv_sec; tv->tv_usec = (long)t.tv_usec; }

/* Byte ordering */
#ifdef __BYTE_ORDER__
#define G_BYTE_ORDER __BYTE_ORDER__
#define G_BIG_ENDIAN __ORDER_BIG_ENDIAN__
#else
#define G_BYTE_ORDER 1234
#define G_BIG_ENDIAN 4321
#endif

#if G_BYTE_ORDER == G_BIG_ENDIAN
#define GINT16_FROM_LE(x) (int16_t)(((uint16_t)(x) >> 8) | ((uint16_t)(x) << 8))
#define GINT32_FROM_LE(x) (int32_t)((FLUID_LE16TOH(x) << 16) | (FLUID16_LE16TOH(x >> 16)))
#else
#define GINT32_FROM_LE(x) (x)
#define GINT16_FROM_LE(x) (x)
#endif

/* Thread support */
#define g_thread_supported() 1
#define g_thread_init(_) do {} while (0)
#define g_usleep(usecs) usleep(usecs)

typedef gpointer (*GThreadFunc)(void *data);
typedef struct
{
    GThreadFunc func;
    void *data;
    pthread_t handle;
    int joinable;
} GThread;

#define g_thread_create fluid_g_thread_create
#define g_thread_join fluid_g_thread_join
GThread *fluid_g_thread_create(GThreadFunc func, void *data, BOOL joinable, GError **error);
void fluid_g_thread_join(GThread *thread);

/* Regular mutex */
typedef pthread_mutex_t GStaticMutex;
#define G_STATIC_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define g_static_mutex_init(_m) pthread_mutex_init(_m, NULL)
#define g_static_mutex_free(_m) pthread_mutex_destroy(_m)
#define g_static_mutex_lock(_m) pthread_mutex_lock(_m)
#define g_static_mutex_unlock(_m) pthread_mutex_unlock(_m)

/* Recursive lock capable mutex */
typedef pthread_mutex_t GStaticRecMutex;
void fluid_g_static_rec_mutex_init(GStaticRecMutex *m);
#define g_static_rec_mutex_init(_m) fluid_g_static_rec_mutex_init(_m)
#define g_static_rec_mutex_free(_m) pthread_mutex_destroy(_m)
#define g_static_rec_mutex_lock(_m) pthread_mutex_lock(_m)
#define g_static_rec_mutex_unlock(_m) pthread_mutex_unlock(_m)

/* Dynamically allocated mutex suitable for fluid_cond_t use */
typedef pthread_mutex_t GMutex;
#define g_mutex_free(m) do { if (m != NULL) { pthread_mutex_destroy(m); g_free(m); } } while(0)
#define g_mutex_lock(m) pthread_mutex_lock(m)
#define g_mutex_unlock(m) pthread_mutex_unlock(m)

static inline GMutex *g_mutex_new(void)
{
    GMutex *mutex = g_new(GMutex, 1);
    pthread_mutex_init(mutex, NULL);
    return mutex;
}

/* Thread condition signaling */
typedef pthread_cond_t GCond;
#define g_cond_free(cond) do { if (cond != NULL) { pthread_cond_destroy(cond); g_free(cond); } } while (0)
#define g_cond_signal(cond) pthread_cond_signal(cond)
#define g_cond_broadcast(cond) pthread_cond_broadcast(cond)
#define g_cond_wait(cond, mutex) pthread_cond_wait(cond, mutex)

static inline GCond *g_cond_new(void)
{
    GCond *cond = g_new(GCond, 1);
    pthread_cond_init(cond, NULL);
    return cond;
}

/* Thread private data */
typedef pthread_key_t GStaticPrivate;
#define g_static_private_init(_priv) do { pthread_key_create(_priv, NULL); } while (0)
#define g_static_private_get(_priv) pthread_getspecific(*_priv)
#define g_static_private_set(_priv, _data, _) pthread_setspecific(*_priv, _data)
#define g_static_private_free(_priv) pthread_key_delete(*_priv)

/* Atomic operations */
#define g_atomic_int_inc(_pi) __sync_fetch_and_add(_pi, 1)
#define g_atomic_int_get(_pi) (__sync_synchronize(), *(_pi))
#define g_atomic_int_set(_pi, _val) do { __sync_synchronize(); *(_pi) = _val; __sync_synchronize(); } while (0)
#define g_atomic_int_dec_and_test(_pi) (__sync_sub_and_fetch(_pi, 1) == 0)
#define g_atomic_int_compare_and_exchange(_pi, _old, _new) __sync_bool_compare_and_swap(_pi, _old, _new)
#define g_atomic_int_exchange_and_add(_pi, _add) __sync_fetch_and_add(_pi, _add)

#endif

#endif
