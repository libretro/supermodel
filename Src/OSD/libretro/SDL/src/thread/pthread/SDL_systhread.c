/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2016 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "../../SDL_internal.h"

#include <pthread.h>

#if HAVE_PTHREAD_NP_H
#include <pthread_np.h>
#endif

#include <signal.h>

#ifdef __LINUX__
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif /* __LINUX__ */

#if defined(__LINUX__) || defined(__APPLE__)
#include <dlfcn.h>
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT NULL
#endif
#endif

#include "SDL_platform.h"
#include "SDL_thread.h"
#include "../SDL_thread_c.h"
#include "../SDL_systhread.h"

#ifdef __HAIKU__
#include <be/kernel/OS.h>
#endif

#include "assert.h"

static void *
RunThread(void *data)
{
    SDL_RunThread(data);
    return NULL;
}

#if defined(__APPLE__)
static SDL_bool checked_setname = SDL_FALSE;
static int (*ppthread_setname_np)(const char*) = NULL;
#elif defined(__LINUX__)
static SDL_bool checked_setname = SDL_FALSE;
static int (*ppthread_setname_np)(pthread_t, const char*) = NULL;
#endif
int
SDL_SYS_CreateThread(SDL_Thread *thread, void *args)
{
	pthread_attr_t type;

    /* do this here before any threads exist, so there's no race condition. */
    #if defined(__APPLE__) || defined(__LINUX__)
    if (!checked_setname) {
        void *fn = dlsym(RTLD_DEFAULT, "pthread_setname_np");
        #if defined(__APPLE__)
        ppthread_setname_np = (int(*)(const char*)) fn;
        #elif defined(__LINUX__)
        ppthread_setname_np = (int(*)(pthread_t, const char*)) fn;
        #endif
        checked_setname = SDL_TRUE;
    }
    #endif

    /* Set the thread attributes */
    if (pthread_attr_init(&type) != 0) {
        return SDL_SetError("Couldn't initialize pthread attributes");
    }
    pthread_attr_setdetachstate(&type, PTHREAD_CREATE_JOINABLE);
    
    /* Set caller-requested stack size. Otherwise: use the system default. */
    if (thread->stacksize) {
        pthread_attr_setstacksize(&type, (size_t) thread->stacksize);
    }

    /* Create the thread and go! */
    if (pthread_create(&thread->handle, &type, RunThread, args) != 0) {
		// 2017.02.07 - RJS ADD - Logging.
        return SDL_SetError("Not enough resources to create thread");
    }

	// 2017.02.07 - RJS ADD - Logging.

    return 0;
}

void
SDL_SYS_SetupThread(const char *name)
{
    if (name != NULL) {
        #if defined(__APPLE__) || defined(__LINUX__)
        assert(checked_setname);
        if (ppthread_setname_np != NULL) {
            #if defined(__APPLE__)
            ppthread_setname_np(name);
            #elif defined(__LINUX__)
            ppthread_setname_np(pthread_self(), name);
            #endif
        }
        #elif HAVE_PTHREAD_SETNAME_NP
            #if defined(__NETBSD__)
            pthread_setname_np(pthread_self(), "%s", name);
            #else
            pthread_setname_np(pthread_self(), name);
            #endif
        #elif HAVE_PTHREAD_SET_NAME_NP
            pthread_set_name_np(pthread_self(), name);
        #elif defined(__HAIKU__)
            /* The docs say the thread name can't be longer than B_OS_NAME_LENGTH. */
            char namebuf[B_OS_NAME_LENGTH];
            SDL_snprintf(namebuf, sizeof (namebuf), "%s", name);
            namebuf[sizeof (namebuf) - 1] = '\0';
            rename_thread(find_thread(NULL), namebuf);
        #endif
    }

#ifdef PTHREAD_CANCEL_ASYNCHRONOUS
    /* Allow ourselves to be asynchronously cancelled */
    {
        int oldstate;
        pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldstate);
    }
#endif
}

SDL_threadID
SDL_ThreadID(void)
{
    return ((SDL_threadID) pthread_self());
}

int
SDL_SYS_SetThreadPriority(SDL_ThreadPriority priority)
{
#if __NACL__ 
    /* FIXME: Setting thread priority does not seem to be supported in NACL */
    return 0;
#elif __LINUX__
    int value;

    if (priority == SDL_THREAD_PRIORITY_LOW) {
        value = 19;
    } else if (priority == SDL_THREAD_PRIORITY_HIGH) {
        value = -20;
    } else {
        value = 0;
    }
    if (setpriority(PRIO_PROCESS, syscall(SYS_gettid), value) < 0) {
        /* Note that this fails if you're trying to set high priority
           and you don't have root permission. BUT DON'T RUN AS ROOT!

           You can grant the ability to increase thread priority by
           running the following command on your application binary:
               sudo setcap 'cap_sys_nice=eip' <application>
         */
        return SDL_SetError("setpriority() failed");
    }
    return 0;
#else
    struct sched_param sched;
    int policy;
    pthread_t thread = pthread_self();

    if (pthread_getschedparam(thread, &policy, &sched) < 0) {
        return SDL_SetError("pthread_getschedparam() failed");
    }
    if (priority == SDL_THREAD_PRIORITY_LOW) {
        sched.sched_priority = sched_get_priority_min(policy);
    } else if (priority == SDL_THREAD_PRIORITY_HIGH) {
        sched.sched_priority = sched_get_priority_max(policy);
    } else {
        int min_priority = sched_get_priority_min(policy);
        int max_priority = sched_get_priority_max(policy);
        sched.sched_priority = (min_priority + (max_priority - min_priority) / 2);
    }
    if (pthread_setschedparam(thread, policy, &sched) < 0) {
        return SDL_SetError("pthread_setschedparam() failed");
    }
    return 0;
#endif /* linux */
}

void
SDL_SYS_WaitThread(SDL_Thread * thread)
{
    pthread_join(thread->handle, 0);
}

void
SDL_SYS_DetachThread(SDL_Thread * thread)
{
    pthread_detach(thread->handle);
}

/* vi: set ts=4 sw=4 expandtab: */
