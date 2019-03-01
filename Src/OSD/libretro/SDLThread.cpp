/**
 ** Supermodel
 ** A Sega Model 3 Arcade Emulator.
 ** Copyright 2011 Bart Trzynadlowski, Nik Henson
 **
 ** This file is part of Supermodel.
 **
 ** Supermodel is free software: you can redistribute it and/or modify it under
 ** the terms of the GNU General Public License as published by the Free 
 ** Software Foundation, either version 3 of the License, or (at your option)
 ** any later version.
 **
 ** Supermodel is distributed in the hope that it will be useful, but WITHOUT
 ** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 ** FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 ** more details.
 **
 ** You should have received a copy of the GNU General Public License along
 ** with Supermodel.  If not, see <http://www.gnu.org/licenses/>.
 **/
 
/*
 * Thread.cpp
 * 
 * SDL-based implementation of threading primitives.
 */
 
#include "Supermodel.h"
#include "../Thread.h"
#include <SDL_thread.h>

#define SDL_MUTEX_TIMEDOUT 1

#define SDL_MUTEX_MAXWAIT (~(uint32_t)0)

struct SDL_semaphore;
typedef struct SDL_semaphore SDL_sem;

#ifdef _WIN32
#include <windows.h>

struct SDL_semaphore
{
    HANDLE id;
    LONG count;
};

SDL_sem *
SDL_CreateSemaphore(uint32_t initial_value)
{
    /* Allocate sem memory */
    SDL_sem *sem = (SDL_sem *)malloc(sizeof(*sem));

    if (sem)
    {
       /* Create the semaphore, with max value 32K */
#if __WINRT__
       sem->id = CreateSemaphoreEx(NULL, initial_value, 32 * 1024, NULL, 0, SEMAPHORE_ALL_ACCESS);
#else
       sem->id = CreateSemaphore(NULL, initial_value, 32 * 1024, NULL);
#endif
       sem->count = initial_value;
       if (!sem->id)
       {
          free(sem);
          sem = NULL;
       }
    }

    return (sem);
}

/* Free the semaphore */
void
SDL_DestroySemaphore(SDL_sem * sem)
{
   if (!sem)
      return;

   if (sem->id)
   {
      CloseHandle(sem->id);
      sem->id = 0;
   }
   free(sem);
}

int
SDL_SemWaitTimeout(SDL_sem * sem, uint32_t timeout)
{
    int retval;
    DWORD dwMilliseconds;

    if (!sem)
       return -1;

    if (timeout == SDL_MUTEX_MAXWAIT)
        dwMilliseconds = INFINITE;
    else
        dwMilliseconds = (DWORD) timeout;
#if __WINRT__
    switch (WaitForSingleObjectEx(sem->id, dwMilliseconds, FALSE)) {
#else
    switch (WaitForSingleObject(sem->id, dwMilliseconds)) {
#endif
    case WAIT_OBJECT_0:
        InterlockedDecrement(&sem->count);
        retval = 0;
        break;
    case WAIT_TIMEOUT:
        retval = SDL_MUTEX_TIMEDOUT;
        break;
    default:
        retval = -1;
        break;
    }
    return retval;
}

int
SDL_SemTryWait(SDL_sem * sem)
{
    return SDL_SemWaitTimeout(sem, 0);
}

int
SDL_SemWait(SDL_sem * sem)
{
    return SDL_SemWaitTimeout(sem, SDL_MUTEX_MAXWAIT);
}

/* Returns the current count of the semaphore */
uint32_t
SDL_SemValue(SDL_sem * sem)
{
    if (!sem)
        return 0;
    return (uint32_t)sem->count;
}

int
SDL_SemPost(SDL_sem * sem)
{
    if (!sem)
        return -1;
    /* Increase the counter in the first place, because
     * after a successful release the semaphore may
     * immediately get destroyed by another thread which
     * is waiting for this semaphore.
     */
    InterlockedIncrement(&sem->count);
    if (ReleaseSemaphore(sem->id, 1, NULL) == FALSE)
    {
       InterlockedDecrement(&sem->count);      /* restore */
       return -1;
    }
    return 0;
}
#else
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>

struct SDL_semaphore
{
    sem_t sem;
};

/* Create a semaphore, initialized with value */
SDL_sem *
SDL_CreateSemaphore(uint32_t initial_value)
{
	SDL_sem *sem = (SDL_sem *)malloc(sizeof(SDL_sem));
    if (sem)
    {
       if (sem_init(&sem->sem, 0, initial_value) < 0)
       {
          free(sem);
          sem = NULL;
       }
    }
    else
        SDL_OutOfMemory();

	return sem;
}

void
SDL_DestroySemaphore(SDL_sem * sem)
{
    if (sem) {
        sem_destroy(&sem->sem);
        free(sem);
    }
}

int
SDL_SemTryWait(SDL_sem * sem)
{
    int retval;

    if (!sem)
       return -1;
    retval = SDL_MUTEX_TIMEDOUT;
    if (sem_trywait(&sem->sem) == 0)
        retval = 0;
    return retval;
}

int
SDL_SemWait(SDL_sem * sem)
{
	int retval;

    if (!sem)
        return -1;

    retval = sem_wait(&sem->sem);
    if (retval < 0)
        retval = -1;

	return retval;
}

int
SDL_SemWaitTimeout(SDL_sem * sem, uint32_t timeout)
{
    int retval;
#ifdef HAVE_SEM_TIMEDWAIT
#ifndef HAVE_CLOCK_GETTIME
    struct timeval now;
#endif
    struct timespec ts_timeout;
#else
    uint32_t end;
#endif

    if (!sem)
       return -1;

    /* Try the easy cases first */
    if (timeout == 0)
        return SDL_SemTryWait(sem);
    if (timeout == SDL_MUTEX_MAXWAIT)
        return SDL_SemWait(sem);

#ifdef HAVE_SEM_TIMEDWAIT
    /* Setup the timeout. sem_timedwait doesn't wait for
    * a lapse of time, but until we reach a certain time.
    * This time is now plus the timeout.
    */
#ifdef HAVE_CLOCK_GETTIME
    clock_gettime(CLOCK_REALTIME, &ts_timeout);

    /* Add our timeout to current time */
    ts_timeout.tv_nsec += (timeout % 1000) * 1000000;
    ts_timeout.tv_sec += timeout / 1000;
#else
    gettimeofday(&now, NULL);

    /* Add our timeout to current time */
    ts_timeout.tv_sec = now.tv_sec + (timeout / 1000);
    ts_timeout.tv_nsec = (now.tv_usec + (timeout % 1000) * 1000) * 1000;
#endif

    /* Wrap the second if needed */
    if (ts_timeout.tv_nsec > 1000000000) {
        ts_timeout.tv_sec += 1;
        ts_timeout.tv_nsec -= 1000000000;
    }

    /* Wait. */
    do {
        retval = sem_timedwait(&sem->sem, &ts_timeout);
    } while (retval < 0 && errno == EINTR);

    if (retval < 0)
    {
       if (errno == ETIMEDOUT)
          retval = SDL_MUTEX_TIMEDOUT;

    }
#else
    end = SDL_GetTicks() + timeout;
    while ((retval = SDL_SemTryWait(sem)) == SDL_MUTEX_TIMEDOUT)
    {
       if (SDL_TICKS_PASSED(SDL_GetTicks(), end))
          break;
       SDL_Delay(1);
    }
#endif /* HAVE_SEM_TIMEDWAIT */

    return retval;
}

uint32_t
SDL_SemValue(SDL_sem * sem)
{
    int ret = 0;
    if (sem)
    {
       sem_getvalue(&sem->sem, &ret);
       if (ret < 0)
          ret = 0;
    }
    return (uint32_t) ret;
}

int
SDL_SemPost(SDL_sem * sem)
{
   if (!sem)
      return -1;

   return sem_post(&sem->sem);
}
#endif

#ifdef _WIN32

#ifdef _XBOX
#include <xtl.h>
#else
#include <windows.h>
#endif
#include <mmsystem.h>

/* The first (low-resolution) ticks value of the application */
static DWORD start = 0;
static BOOL ticks_started = FALSE; 

/* Store if a high-resolution performance counter exists on the system */
static BOOL hires_timer_available;
/* The first high-resolution ticks value of the application */
static LARGE_INTEGER hires_start_ticks;
/* The number of ticks per second of the high-resolution performance counter */
static LARGE_INTEGER hires_ticks_per_second;

static void
SDL_SetSystemTimerResolution(const UINT uPeriod)
{
#ifndef __WINRT__
    static UINT timer_period = 0;

    if (uPeriod != timer_period) {
        if (timer_period) {
            timeEndPeriod(timer_period);
        }

        timer_period = uPeriod;

        if (timer_period) {
            timeBeginPeriod(timer_period);
        }
    }
#endif
}

static void
SDL_TimerResolutionChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    UINT uPeriod;

    /* Unless the hint says otherwise, let's have good sleep precision */
    if (hint && *hint) {
        uPeriod = atoi(hint);
    } else {
        uPeriod = 1;
    }
    if (uPeriod || oldValue != hint) {
        SDL_SetSystemTimerResolution(uPeriod);
    }
}

void
SDL_TicksInit(void)
{
    if (ticks_started)
        return;
    ticks_started = true;

    /* Set first ticks value */
    /* QueryPerformanceCounter has had problems in the past, but lots of games
       use it, so we'll rely on it here.
     */
    if (QueryPerformanceFrequency(&hires_ticks_per_second) == TRUE) {
        hires_timer_available = TRUE;
        QueryPerformanceCounter(&hires_start_ticks);
    } else {
        hires_timer_available = FALSE;
#ifndef __WINRT__
        start = timeGetTime();
#endif /* __WINRT__ */
    }
}

void
SDL_TicksQuit(void)
{
    SDL_SetSystemTimerResolution(0);  /* always release our timer resolution request. */

    start = 0;
    ticks_started = false;
}

uint32_t
SDL_GetTicks(void)
{
    DWORD now = 0;
    LARGE_INTEGER hires_now;

    if (!ticks_started) {
        SDL_TicksInit();
    }

    if (hires_timer_available) {
        QueryPerformanceCounter(&hires_now);

        hires_now.QuadPart -= hires_start_ticks.QuadPart;
        hires_now.QuadPart *= 1000;
        hires_now.QuadPart /= hires_ticks_per_second.QuadPart;

        return (DWORD) hires_now.QuadPart;
    } else {
#ifndef __WINRT__
        now = timeGetTime();
#endif /* __WINRT__ */
    }

    return (now - start);
}

void SDL_Delay(uint32_t ms)
{
    /* Sleep() is not publicly available to apps in early versions of WinRT.
     *
     * Visual C++ 2013 Update 4 re-introduced Sleep() for Windows 8.1 and
     * Windows Phone 8.1.
     *
     * Use the compiler version to determine availability.
     *
     * NOTE #1: _MSC_FULL_VER == 180030723 for Visual C++ 2013 Update 3.
     * NOTE #2: Visual C++ 2013, when compiling for Windows 8.0 and
     *    Windows Phone 8.0, uses the Visual C++ 2012 compiler to build
     *    apps and libraries.
     */
#if defined(__WINRT__) && defined(_MSC_FULL_VER) && (_MSC_FULL_VER <= 180030723)
    static HANDLE mutex = 0;
    if (!mutex)
        mutex = CreateEventEx(0, 0, 0, EVENT_ALL_ACCESS);
    WaitForSingleObjectEx(mutex, ms, FALSE);
#else
    if (!ticks_started)
        SDL_TicksInit();

    Sleep(ms);
#endif
}

/* vi: set ts=4 sw=4 expandtab: */
#else
#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>

#include "assert.h"

/* The clock_gettime provides monotonous time, so we should use it if
   it's available. The clock_gettime function is behind ifdef
   for __USE_POSIX199309
   Tommi Kyntola (tommi.kyntola@ray.fi) 27/09/2005
*/
/* Reworked monotonic clock to not assume the current system has one
   as not all linux kernels provide a monotonic clock (yeah recent ones
   probably do)
   Also added OS X Monotonic clock support
   Based on work in https://github.com/ThomasHabets/monotonic_clock
 */
#include <time.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

/* Use CLOCK_MONOTONIC_RAW, if available, which is not subject to adjustment by NTP */
#if HAVE_CLOCK_GETTIME
#ifdef CLOCK_MONOTONIC_RAW
#define SDL_MONOTONIC_CLOCK CLOCK_MONOTONIC_RAW
#else
#define SDL_MONOTONIC_CLOCK CLOCK_MONOTONIC
#endif
#endif

/* The first ticks value of the application */
#if HAVE_CLOCK_GETTIME
static struct timespec start_ts;
#elif defined(__APPLE__)
static uint64_t start_mach;
mach_timebase_info_data_t mach_base_info;
#endif
static bool has_monotonic_time = false;
static struct timeval start_tv;
static bool ticks_started = false;

void
SDL_TicksInit(void)
{
    if (ticks_started)
        return;
    ticks_started = true;

    /* Set first ticks value */
#if HAVE_CLOCK_GETTIME
    if (clock_gettime(SDL_MONOTONIC_CLOCK, &start_ts) == 0)
        has_monotonic_time = true;
    else
#elif defined(__APPLE__)
    kern_return_t ret = mach_timebase_info(&mach_base_info);
    if (ret == 0) {
        has_monotonic_time = true;
        start_mach = mach_absolute_time();
    } else
#endif
    {
        gettimeofday(&start_tv, NULL);
    }
}

void
SDL_TicksQuit(void)
{
    ticks_started = false;
}

uint32_t
SDL_GetTicks(void)
{
	uint32_t ticks = 0;
    if (!ticks_started)
        SDL_TicksInit();

    if (has_monotonic_time) {
#if HAVE_CLOCK_GETTIME
       struct timespec now;
       clock_gettime(SDL_MONOTONIC_CLOCK, &now);
       ticks = (now.tv_sec - start_ts.tv_sec) * 1000 + (now.tv_nsec -
             start_ts.tv_nsec) / 1000000;
#elif defined(__APPLE__)
       uint64_t now = mach_absolute_time();
       ticks = (uint32_t)((((now - start_mach) * mach_base_info.numer) / mach_base_info.denom) / 1000000);
#else
       assert(false);
       ticks = 0;
#endif
    } else {
        struct timeval now;

		gettimeofday(&now, NULL);
		ticks = (uint32_t)((now.tv_sec - start_tv.tv_sec) * 1000 + (now.tv_usec - start_tv.tv_usec) / 1000);
	}
	return (ticks);
}

void
SDL_Delay(uint32_t ms)
{
	int was_error = 0;
	struct timespec elapsed, tv;

	elapsed.tv_sec = ms / 1000;
	elapsed.tv_nsec = (ms % 1000) * 1000000;

	do
	{
		errno = 0;

		tv.tv_sec	= elapsed.tv_sec;
		tv.tv_nsec	= elapsed.tv_nsec;

		was_error	= nanosleep(&tv, &elapsed);
	} while (was_error && (errno == EINTR));
}

#endif

void CThread::Sleep(UINT32 ms)
{
	SDL_Delay(ms);
}

UINT32 CThread::GetTicks()
{
	return SDL_GetTicks();
}

CThread *CThread::CreateThread(ThreadStart start, void *startParam)
{
	SDL_Thread *impl = SDL_CreateThread(start, "x", startParam);
	if (impl == NULL)
		return NULL;
	return new CThread(impl);
}

CSemaphore *CThread::CreateSemaphore(UINT32 initVal)
{
	SDL_sem *impl = SDL_CreateSemaphore(initVal);
	if (impl == NULL)
		return NULL;
	return new CSemaphore(impl);
}

CCondVar *CThread::CreateCondVar()
{
	SDL_cond *impl = SDL_CreateCond();
	if (impl == NULL)
		return NULL;
	return new CCondVar(impl);
}

CMutex *CThread::CreateMutex()
{
	SDL_mutex *impl = SDL_CreateMutex();
	if (impl == NULL)
		return NULL;
	return new CMutex(impl);
}

const char *CThread::GetLastError()
{
	return SDL_GetError();
}

CThread::CThread(void *impl) : m_impl(impl)
{
	//
}

CThread::~CThread()
{
	Kill();
}

UINT32 CThread::GetId()
{
	return SDL_GetThreadID((SDL_Thread*)m_impl);
}

void CThread::Kill()
{
	if (m_impl != NULL)
		SDL_KillThread((SDL_Thread*)m_impl);
	m_impl = NULL;
}

int CThread::Wait()
{
	int status;
	if (m_impl == NULL)
		return -1;
	SDL_WaitThread((SDL_Thread*)m_impl, &status);
	m_impl = NULL;
	return status;
}

CSemaphore::CSemaphore(void *impl) : m_impl(impl)
{
	//
}

CSemaphore::~CSemaphore()
{
	SDL_DestroySemaphore((SDL_sem*)m_impl);
}

UINT32 CSemaphore::GetValue()
{
	return SDL_SemValue((SDL_sem*)m_impl);
}

bool CSemaphore::Wait()
{
	return SDL_SemWait((SDL_sem*)m_impl) == 0;
}

bool CSemaphore::Post()
{
	return SDL_SemPost((SDL_sem*)m_impl) == 0;
}

CCondVar::CCondVar(void *impl) : m_impl(impl)
{
	//
}

CCondVar::~CCondVar()
{
	SDL_DestroyCond((SDL_cond*)m_impl);
}

bool CCondVar::Wait(CMutex *mutex)
{
	return SDL_CondWait((SDL_cond*)m_impl, (SDL_mutex*)mutex->m_impl) == 0;
}

bool CCondVar::Signal()
{
	return SDL_CondSignal((SDL_cond*)m_impl) == 0;
}

bool CCondVar::SignalAll()
{
	return SDL_CondBroadcast((SDL_cond*)m_impl) == 0;
}

CMutex::CMutex(void *impl) : m_impl(impl)
{
	//
}

CMutex::~CMutex()
{
	SDL_DestroyMutex((SDL_mutex*)m_impl);
}

bool CMutex::Lock()
{
	return SDL_mutexP((SDL_mutex*)m_impl) == 0;
}

bool CMutex::Unlock()
{
	return SDL_mutexV((SDL_mutex*)m_impl) == 0;
}
