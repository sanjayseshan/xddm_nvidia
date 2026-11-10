#define _WIN32_WINNT 0x0502
#define WINVER 0x0502
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "../mesa/src/c11/impl/nvx_win32_xp.h"

static CONDITION_VARIABLE condition;
static CRITICAL_SECTION condition_mutex;
static volatile LONG condition_ready;
static volatile LONG condition_passed;
static nvx_xp_rwlock_state *rwlock;
static volatile LONG shared_value;

static DWORD WINAPI condition_waiter(void *unused)
{
   (void)unused;
   EnterCriticalSection(&condition_mutex);
   InterlockedIncrement(&condition_ready);
   if (nvx_xp_condition_wait(&condition, &condition_mutex, 5000))
      InterlockedIncrement(&condition_passed);
   LeaveCriticalSection(&condition_mutex);
   return 0;
}

static DWORD WINAPI writer(void *unused)
{
   (void)unused;
   for (int i = 0; i < 2000; i++) {
      nvx_xp_rwlock_wrlock((void **)&rwlock);
      shared_value++;
      nvx_xp_rwlock_wrunlock((void **)&rwlock);
   }
   return 0;
}

static DWORD WINAPI reader(void *unused)
{
   (void)unused;
   LONG previous = -1;
   for (int i = 0; i < 4000; i++) {
      nvx_xp_rwlock_rdlock((void **)&rwlock);
      LONG current = shared_value;
      if (current < previous)
         InterlockedExchange(&condition_passed, -1000);
      previous = current;
      nvx_xp_rwlock_rdunlock((void **)&rwlock);
   }
   return 0;
}

int main(void)
{
   HANDLE threads[8];
   DWORD wait_result;

   InitializeCriticalSection(&condition_mutex);
   if (!nvx_xp_condition_init(&condition))
      return 1;
   for (int i = 0; i < 3; i++) {
      threads[i] = CreateThread(NULL, 0, condition_waiter, NULL, 0, NULL);
      if (!threads[i])
         return 2;
   }
   while (condition_ready != 3)
      Sleep(1);
   EnterCriticalSection(&condition_mutex);
   nvx_xp_condition_wake(&condition, TRUE);
   LeaveCriticalSection(&condition_mutex);
   wait_result = WaitForMultipleObjects(3, threads, TRUE, 6000);
   for (int i = 0; i < 3; i++)
      CloseHandle(threads[i]);
   nvx_xp_condition_destroy(&condition);
   DeleteCriticalSection(&condition_mutex);
   if (wait_result != WAIT_OBJECT_0 || condition_passed != 3)
      return 3;

   rwlock = calloc(1, sizeof(*rwlock));
   if (!rwlock)
      return 4;
   InitializeCriticalSection(&rwlock->guard);
   rwlock->readers_done = CreateSemaphore(NULL, 0, 0x7fffffff, NULL);
   rwlock->writers_done = CreateSemaphore(NULL, 0, 0x7fffffff, NULL);
   if (!rwlock->readers_done || !rwlock->writers_done)
      return 5;
   for (int i = 0; i < 4; i++)
      threads[i] = CreateThread(NULL, 0, writer, NULL, 0, NULL);
   for (int i = 4; i < 8; i++)
      threads[i] = CreateThread(NULL, 0, reader, NULL, 0, NULL);
   wait_result = WaitForMultipleObjects(8, threads, TRUE, 15000);
   for (int i = 0; i < 8; i++)
      CloseHandle(threads[i]);
   if (wait_result != WAIT_OBJECT_0 || shared_value != 8000 ||
       condition_passed != 3)
      return 6;

   CloseHandle(rwlock->readers_done);
   CloseHandle(rwlock->writers_done);
   DeleteCriticalSection(&rwlock->guard);
   free(rwlock);
   puts("XP condition and shared/exclusive lock smoke tests passed");
   return 0;
}
