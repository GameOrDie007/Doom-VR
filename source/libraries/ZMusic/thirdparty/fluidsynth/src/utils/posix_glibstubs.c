/* The glib pieces this FluidSynth uses, on POSIX threads (posix_glibstubs.h). */
#if defined(__ANDROID__) || defined(__linux__)

#include "fluidsynth_priv.h"
#include "fluid_sys.h"
#include "posix_glibstubs.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

BOOL fluid_g_file_test(const char *path, int flags)
{
    struct stat st;
    if (path == NULL || stat(path, &st) != 0)
    {
        return FALSE;
    }
    if (flags & G_FILE_TEST_EXISTS)
    {
        return TRUE;
    }
    if (flags & G_FILE_TEST_IS_REGULAR)
    {
        return S_ISREG(st.st_mode) ? TRUE : FALSE;
    }
    return FALSE;
}

double fluid_g_get_monotonic_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0; /* time in micros */
}

void fluid_g_static_rec_mutex_init(GStaticRecMutex *m)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
}

/* Thread support */
static void *g_thread_wrapper(void *info_)
{
    GThread *info = (GThread *)info_;
    info->func(info->data);
    /* Free the "GThread" now if it was detached. Otherwise, it's freed in fluid_g_thread_join. */
    if (!info->joinable)
    {
        free(info);
    }
    return NULL;
}

GThread *fluid_g_thread_create(GThreadFunc func, void *data, BOOL joinable, GError **error)
{
    static GError error_container;

    g_return_val_if_fail(func != NULL, NULL);

    GThread *info = (GThread *)malloc(sizeof(GThread));
    if (info == NULL)
    {
        return NULL;
    }

    info->func = func;
    info->data = data;
    info->joinable = joinable ? 1 : 0;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (!joinable)
    {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    }
    const int rc = pthread_create(&info->handle, &attr, g_thread_wrapper, info);
    pthread_attr_destroy(&attr);

    if (error != NULL)
    {
        error_container.code = rc;
        error_container.message = rc != 0 ? strerror(rc) : NULL;
        *error = &error_container;
    }

    if (rc != 0)
    {
        free(info);
        info = NULL;
    }
    return info;
}

void fluid_g_thread_join(GThread *thread)
{
    if (thread != NULL && thread->joinable)
    {
        pthread_join(thread->handle, NULL);
        free(thread);
    }
}

#endif
