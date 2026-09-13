/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (retro_eventcount.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* struct timespec and syscall() are POSIX/glibc surface that a strict
 * C89 compile does not expose by default; libretro-common builds as
 * C89, and rthreads.c asks for the same baseline for the same reason. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <rthreads/retro_eventcount.h>
#include <rthreads/rthreads.h>

/* The address-wait backends hand the kernel a bare 32-bit word, so the
 * epoch must be exactly that.  C89 has no static assertion; a negative
 * array bound is the portable spelling. */
typedef char retro_eventcount_epoch_is_a_word_
   [(sizeof(retro_atomic_int_t) == sizeof(int)) ? 1 : -1];

/* Backend selection.
 *
 * RETRO_EC_ADDR_LINUX and RETRO_EC_ADDR_WIN32 name builds that can park
 * a thread on a bare word.  Both still need the atomics to be lock-free
 * for the handshake in notify to hold, so a build that lands on the
 * volatile fallback drops to the locked backend regardless of platform.
 *
 * RETRO_EVENTCOUNT_FORCE_SCOND selects the condition-variable backend on
 * any target, so a host with futex can still build and test the path the
 * console and Apple ports take.  It mirrors retro_atomic.h's
 * RETRO_ATOMIC_FORCE_* overrides and exists for the same reason.
 */
#if defined(RETRO_ATOMIC_LOCK_FREE) && !defined(RETRO_EVENTCOUNT_FORCE_SCOND)
#if defined(__linux__) && !defined(ANDROID_NO_FUTEX)
#define RETRO_EC_ADDR_LINUX 1
#elif defined(_WIN32) && !defined(_XBOX)
#define RETRO_EC_ADDR_WIN32 1
#endif
#endif

#if defined(RETRO_EC_ADDR_LINUX)
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/syscall.h>
#include <linux/futex.h>

/* Older kernel headers predate the _PRIVATE flags; the syscall has
 * carried them since 2.6.22 and the numbers are ABI. */
#ifndef FUTEX_WAIT_PRIVATE
#define FUTEX_WAIT_PRIVATE 128
#endif
#ifndef FUTEX_WAKE_PRIVATE
#define FUTEX_WAKE_PRIVATE 129
#endif
#endif

#if defined(RETRO_EC_ADDR_WIN32)
#include <windows.h>

/* WaitOnAddress and friends arrived in Windows 8 and live in an
 * API set the older SDKs have no header for, so the prototypes are
 * spelled out here and the entry points are resolved at runtime.  A
 * binary built this way still starts on 9x and XP; it simply finds
 * nothing and uses the condition variable instead. */
typedef BOOL (WINAPI *ec_wait_on_address_t)(volatile VOID*, PVOID, SIZE_T, DWORD);
typedef VOID (WINAPI *ec_wake_by_address_all_t)(PVOID);

static ec_wait_on_address_t     ec_WaitOnAddress;
static ec_wake_by_address_all_t ec_WakeByAddressAll;
static int                      ec_win32_probed;

static void ec_win32_probe(void)
{
   HMODULE mod;

   if (ec_win32_probed)
      return;

   /* GetModuleHandleA first: in a process that already has the API set
    * loaded this adds no reference to drop, and on 9x both calls simply
    * fail. */
   if (!(mod = GetModuleHandleA("api-ms-win-core-synch-l1-2-0.dll")))
      mod = LoadLibraryA("api-ms-win-core-synch-l1-2-0.dll");

   if (!mod)
      mod = GetModuleHandleA("kernelbase.dll");

   if (mod)
   {
      ec_WaitOnAddress    = (ec_wait_on_address_t)
         GetProcAddress(mod, "WaitOnAddress");
      ec_WakeByAddressAll = (ec_wake_by_address_all_t)
         GetProcAddress(mod, "WakeByAddressAll");
   }

   if (!ec_WaitOnAddress || !ec_WakeByAddressAll)
   {
      ec_WaitOnAddress    = NULL;
      ec_WakeByAddressAll = NULL;
   }

   ec_win32_probed = 1;
}
#endif

/* True when this object parks on the epoch word itself and therefore
 * holds no lock.  A compile-time answer everywhere but Windows, where
 * it follows the runtime probe. */
static INLINE int ec_is_lockless(const retro_eventcount_t *ec)
{
   return ec->lock == NULL;
}

bool retro_eventcount_init(retro_eventcount_t *ec)
{
   int lockless = 0;

   if (!ec)
      return false;

   memset(ec, 0, sizeof(*ec));
   retro_atomic_int_init(&ec->epoch, 0);
   retro_atomic_int_init(&ec->waiters, 0);

#if defined(RETRO_EC_ADDR_LINUX)
   lockless = 1;
#elif defined(RETRO_EC_ADDR_WIN32)
   ec_win32_probe();
   lockless = (ec_WaitOnAddress != NULL);
#endif

   if (lockless)
      return true;

   if (!(ec->lock = slock_new()))
      return false;

   if (!(ec->cond = scond_new()))
   {
      slock_free(ec->lock);
      ec->lock = NULL;
      return false;
   }

   return true;
}

void retro_eventcount_free(retro_eventcount_t *ec)
{
   if (!ec)
      return;

   if (ec->cond)
      scond_free(ec->cond);
   if (ec->lock)
      slock_free(ec->lock);

   ec->cond = NULL;
   ec->lock = NULL;
}

void retro_eventcount_notify(retro_eventcount_t *ec)
{
   /* Publish the state change first.  The seq_cst fence is what orders
    * this store against the waiters load below; an acq_rel
    * read-modify-write does not, and without it a notify could read
    * zero waiters while a consumer that has not yet seen the new epoch
    * is on its way into a park. */
   retro_atomic_fetch_add_int(&ec->epoch, 1);
   retro_atomic_thread_fence_seq_cst();

#if defined(RETRO_ATOMIC_LOCK_FREE)
   /* Only sound where the fence above is a real barrier.  Where it
    * degrades to a compiler barrier the handshake cannot be relied on,
    * so that build takes the lock on every notify instead -- which is
    * the single-core case, where the lock is uncontended anyway. */
   if (retro_atomic_load_acquire_int(&ec->waiters) == 0)
      return;
#endif

#if defined(RETRO_EC_ADDR_LINUX)
   syscall(SYS_futex, (void*)&ec->epoch, FUTEX_WAKE_PRIVATE,
         INT_MAX, NULL, NULL, 0);
#else
#if defined(RETRO_EC_ADDR_WIN32)
   if (ec_is_lockless(ec))
   {
      ec_WakeByAddressAll((PVOID)&ec->epoch);
      return;
   }
#endif
   /* A registered waiter holds the lock from prepare_wait until
    * scond_wait releases it, so taking it here cannot overtake the
    * consumer's own re-check. */
   slock_lock(ec->lock);
   scond_broadcast(ec->cond);
   slock_unlock(ec->lock);
#endif
}

int retro_eventcount_prepare_wait(retro_eventcount_t *ec)
{
   if (!ec_is_lockless(ec))
      slock_lock(ec->lock);

   /* Mirror of notify: register, fence, then read.  Between the two
    * fences it is impossible for a notify to see no waiters and for
    * this thread to read the pre-notify epoch. */
   retro_atomic_fetch_add_int(&ec->waiters, 1);
   retro_atomic_thread_fence_seq_cst();

   return retro_atomic_load_acquire_int(&ec->epoch);
}

void retro_eventcount_cancel_wait(retro_eventcount_t *ec)
{
   retro_atomic_fetch_sub_int(&ec->waiters, 1);

   if (!ec_is_lockless(ec))
      slock_unlock(ec->lock);
}

void retro_eventcount_commit_wait(retro_eventcount_t *ec, int key)
{
#if defined(RETRO_EC_ADDR_LINUX)
   int expect = key;
   if (retro_atomic_load_acquire_int(&ec->epoch) == key)
      syscall(SYS_futex, (void*)&ec->epoch, FUTEX_WAIT_PRIVATE,
            expect, NULL, NULL, 0);
   retro_atomic_fetch_sub_int(&ec->waiters, 1);
#else
#if defined(RETRO_EC_ADDR_WIN32)
   if (ec_is_lockless(ec))
   {
      int expect = key;
      if (retro_atomic_load_acquire_int(&ec->epoch) == key)
         ec_WaitOnAddress((volatile VOID*)&ec->epoch, &expect,
               sizeof(expect), INFINITE);
      retro_atomic_fetch_sub_int(&ec->waiters, 1);
      return;
   }
#endif
   if (retro_atomic_load_acquire_int(&ec->epoch) == key)
      scond_wait(ec->cond, ec->lock);
   retro_atomic_fetch_sub_int(&ec->waiters, 1);
   slock_unlock(ec->lock);
#endif
}

bool retro_eventcount_commit_wait_timeout(retro_eventcount_t *ec,
      int key, int64_t timeout_us)
{
   bool signalled = true;

#if defined(RETRO_EC_ADDR_LINUX)
   if (retro_atomic_load_acquire_int(&ec->epoch) == key)
   {
      struct timespec ts;
      int expect = key;

      ts.tv_sec  = (time_t)(timeout_us / 1000000);
      ts.tv_nsec = (long)((timeout_us % 1000000) * 1000);

      if (syscall(SYS_futex, (void*)&ec->epoch, FUTEX_WAIT_PRIVATE,
               expect, &ts, NULL, 0) != 0 && errno == ETIMEDOUT)
         signalled = false;
   }
   retro_atomic_fetch_sub_int(&ec->waiters, 1);
#else
#if defined(RETRO_EC_ADDR_WIN32)
   if (ec_is_lockless(ec))
   {
      if (retro_atomic_load_acquire_int(&ec->epoch) == key)
      {
         DWORD ms = (DWORD)(timeout_us / 1000);
         int expect = key;

         if (!ec_WaitOnAddress((volatile VOID*)&ec->epoch, &expect,
                  sizeof(expect), ms))
            signalled = false;
      }
      retro_atomic_fetch_sub_int(&ec->waiters, 1);
      return signalled;
   }
#endif
   if (retro_atomic_load_acquire_int(&ec->epoch) == key)
      signalled = scond_wait_timeout(ec->cond, ec->lock, timeout_us);
   retro_atomic_fetch_sub_int(&ec->waiters, 1);
   slock_unlock(ec->lock);
#endif

   return signalled;
}

const char *retro_eventcount_backend_name(void)
{
#if defined(RETRO_EC_ADDR_LINUX)
   return "futex";
#elif defined(RETRO_EC_ADDR_WIN32)
   ec_win32_probe();
   if (ec_WaitOnAddress)
      return "WaitOnAddress";
   return "scond (no WaitOnAddress)";
#elif !defined(RETRO_ATOMIC_LOCK_FREE)
   return "scond (atomics not lock-free)";
#elif defined(RETRO_EVENTCOUNT_FORCE_SCOND)
   return "scond (forced)";
#else
   return "scond";
#endif
}
