/*
 * The contents of this file are subject to the AOLserver Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://aolserver.com/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 * 
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */


/*
 * dns.c --
 *
 *      DNS lookup routines.
 */

static const char *RCSID = "@(#) $Header: /Users/dossy/Desktop/cvs/aolserver/nsd/dns.c,v 1.4.4.1 2002/12/03 14:55:28 schaudhri Exp $, compiled: " __DATE__ " " __TIME__;

#include "nsd.h"

#ifndef INADDR_NONE
#define INADDR_NONE (-1)
#endif

#ifndef NETDB_INTERNAL
#  ifdef h_NETDB_INTERNAL
#  define NETDB_INTERNAL h_NETDB_INTERNAL
#  endif
#endif

/*
 * The following structure maintains a cached lookup value.
 */

#define IP_ADDR_MAX_SIZE 16

typedef struct AddrValue {
    time_t      expires;
    int         entries;
    int         index;
    char        addrs[1][IP_ADDR_MAX_SIZE];
} AddrValue;

typedef struct HostValue {
    time_t      expires;
    char        value[1];
} HostValue;


/*
 * Static variables defined in this file
 */

static Ns_Cache *hostCache;
static Ns_Cache *addrCache;
static Ns_Mutex lock;
static int cachetimeout;

/*
 * Static functions defined in this file
 */

static int GetHost(Ns_DString *dsPtr, char *addr);
static int GetAddr(Ns_DString *dsPtr, char *host, AddrValue **av);
static int DnsGetHost(Ns_DString *dsPtr, Ns_Cache **cachePtr, char *key);
static int DnsGetAddr(Ns_DString *dsPtr, Ns_Cache **cachePtr, char *key);
static void LogError(char *func);


/*
 *----------------------------------------------------------------------
 * Ns_GetHostByAddr, Ns_GetAddrByHost --
 *
 *      Convert an IP address to a hostname or vice versa.
 *
 * Results:
 *  See DnsGet().
 *
 * Side effects:
 *      A new entry is entered into the hash table.
 *
 *----------------------------------------------------------------------
 */

int
Ns_GetHostByAddr(Ns_DString *dsPtr, char *addr)
{
    return DnsGetHost(dsPtr, &hostCache, addr);
}

int
Ns_GetAddrByHost(Ns_DString *dsPtr, char *host)
{
    return DnsGetAddr(dsPtr, &addrCache, host);
}

static int
DnsGetAddr(Ns_DString *dsPtr, Ns_Cache **cachePtr, char *key)
{
    int             new, timeout, status = NS_FALSE;
    AddrValue      *vPtr = NULL;
    Ns_Entry       *ePtr;
    Ns_Cache       *cache;

    /*
     * Get the cache, if enabled.
     */

    Ns_MutexLock(&lock);
    cache = *cachePtr;
    timeout = cachetimeout;
    Ns_MutexUnlock(&lock);

    /*
     * Call GetAddr directly or through cache.
     */

    if (cache == NULL) {
        status = GetAddr(dsPtr, key, NULL);
    } else {
        Ns_CacheLock(cache);
        ePtr = Ns_CacheCreateEntry(cache, key, &new);
        if (!new) {
            while (ePtr != NULL &&
                    (vPtr = Ns_CacheGetValue(ePtr)) == NULL) {
                Ns_CacheWait(cache);
                ePtr = Ns_CacheFindEntry(cache, key);
            }
            if (ePtr) {
                if (vPtr->expires < time(NULL)) {
                    Ns_CacheUnsetValue(ePtr);
                    new = 1;
                } else {
                    Ns_DStringAppend(dsPtr, vPtr->addrs[vPtr->index]);

                    /*
                     * When there are multiple addresses, do round-robin.
                     */

                    if (vPtr->entries > 1) {
                        vPtr->index++;
                        if (vPtr->index == vPtr->entries)
                            vPtr->index = 0;
                    }
                    status = NS_TRUE;
                }
            }
        }
        if (new) {
            Ns_CacheUnlock(cache);
            status = GetAddr(dsPtr, key, &vPtr);
            Ns_CacheLock(cache);
            ePtr = Ns_CacheCreateEntry(cache, key, &new);
            if (status != NS_TRUE) {
                Ns_CacheFlushEntry(ePtr);
            } else {
                Ns_CacheUnsetValue(ePtr);
                vPtr->expires = time(NULL) + timeout;
                Ns_CacheSetValue(ePtr, vPtr);
            }
            Ns_CacheBroadcast(cache);
        }
        Ns_CacheUnlock(cache);
    }
    return status;
}

static int
DnsGetHost(Ns_DString *dsPtr, Ns_Cache **cachePtr, char *key)
{
    int             new, timeout, status = NS_FALSE;
    HostValue      *vPtr = NULL;
    Ns_Entry       *ePtr;
    Ns_Cache       *cache;

    /*
     * Get the cache, if enabled.
     */

    Ns_MutexLock(&lock);
    cache = *cachePtr;
    timeout = cachetimeout;
    Ns_MutexUnlock(&lock);

    /*
     * Call GetHost directly or through cache.
     */

    if (cache == NULL) {
        status = GetHost(dsPtr, key);
    } else {
        Ns_CacheLock(cache);
        ePtr = Ns_CacheCreateEntry(cache, key, &new);
        if (!new) {
            while (ePtr != NULL &&
                    (vPtr = Ns_CacheGetValue(ePtr)) == NULL) {
                Ns_CacheWait(cache);
                ePtr = Ns_CacheFindEntry(cache, key);
            }
            if (ePtr) {
                if (vPtr->expires < time(NULL)) {
                    Ns_CacheUnsetValue(ePtr);
                    new = 1;
                } else {
                    Ns_DStringAppend(dsPtr, vPtr->value);
                    status = NS_TRUE;
                }
            }
        }
        if (new) {
            Ns_CacheUnlock(cache);
            status = GetHost(dsPtr, key);
            Ns_CacheLock(cache);
            ePtr = Ns_CacheCreateEntry(cache, key, &new);
            if (status != NS_TRUE) {
                Ns_CacheFlushEntry(ePtr);
            } else {
                Ns_CacheUnsetValue(ePtr);
                vPtr = ns_malloc(sizeof(HostValue) + dsPtr->length);
                vPtr->expires = time(NULL) + timeout;
                strcpy(vPtr->value, dsPtr->string);
                Ns_CacheSetValue(ePtr, vPtr);
            }
            Ns_CacheBroadcast(cache);
        }
        Ns_CacheUnlock(cache);
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 * NsEnableDNSCache --
 *
 *      Enable DNS results caching.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *  Futher DNS lookups will be cached up to given timeout.
 *
 *----------------------------------------------------------------------
 */

void
NsEnableDNSCache(int timeout)
{
    Ns_MutexLock(&lock);
    cachetimeout = timeout;
    hostCache = Ns_CacheCreate("ns_dnshost", TCL_STRING_KEYS,
    cachetimeout, ns_free);
    addrCache = Ns_CacheCreate("ns_dnsaddr", TCL_STRING_KEYS,
    cachetimeout, ns_free);
    Ns_MutexUnlock(&lock);
}


/*
 *----------------------------------------------------------------------
 * GetHost, GetAddr --
 *
 *      Perform the actual lookup by host or address.
 *
 *  NOTE: A critical section is used instead of a mutex
 *  to ensure waiting on a condition and not mutex spin waiting.
 *
 * Results:
 *      If a name can be found, the function returns NS_TRUE; otherwise, 
 *  it returns NS_FALSE.
 *
 * Side effects:
 *      Result is appended to dsPtr.
 *
 *----------------------------------------------------------------------
 */

static int
GetHost(Ns_DString *dsPtr, char *addr)
{
    struct hostent *he;
    struct sockaddr_in sa;
    static Ns_Cs cs;
    int status = NS_FALSE;

    sa.sin_addr.s_addr = inet_addr(addr);
    if (sa.sin_addr.s_addr != INADDR_NONE) {
    Ns_CsEnter(&cs);
        he = gethostbyaddr((char *) &sa.sin_addr,
               sizeof(struct in_addr), AF_INET);
    if (he == NULL) {
        LogError("gethostbyaddr");
    } else if (he->h_name != NULL) {
        Ns_DStringAppend(dsPtr, he->h_name);
        status = NS_TRUE;
    }
    Ns_CsLeave(&cs);
    }
    return status;
}

static int
GetAddr(Ns_DString *dsPtr, char *host, AddrValue **av)
{
    struct hostent *he;
    struct in_addr  ia;
    AddrValue      *vPtr;
    static Ns_Cs    cs;
    int             status = NS_FALSE;

    Ns_CsEnter(&cs);
    he = gethostbyname(host);
    if ((!he) || (he->h_addrtype != AF_INET) || (!he->h_addr_list[0])) {
        LogError("gethostbyname");
    } else {
        int i, n;
        for (n = 0; he->h_addr_list[n]; n++);
        if (av) {
            vPtr = ns_malloc(sizeof(AddrValue) + (n - 1) * IP_ADDR_MAX_SIZE);
            if (vPtr) {
                vPtr->entries = n;
                vPtr->index   = (n > 1) ? 1 : 0;
                for (i = 0; i < n; i++) {
                    ia.s_addr = ((struct in_addr *) he->h_addr_list[i])->s_addr;
                    strncpy(vPtr->addrs[i], ns_inet_ntoa(ia), IP_ADDR_MAX_SIZE);
                    if (i == 0)
                        Ns_DStringAppend(dsPtr, vPtr->addrs[i]);
                }
                *av = vPtr;
                status = NS_TRUE;
            }
        } else {
            i = (n > 1) ? (Ns_DRand() * n) : 0;
            ia.s_addr = ((struct in_addr *) he->h_addr_list[i])->s_addr;
            Ns_DStringAppend(dsPtr, ns_inet_ntoa(ia));
            status = NS_TRUE;
        }
    }
    Ns_CsLeave(&cs);
    return status;
}


/*
 *----------------------------------------------------------------------
 * LogError -
 *
 *      Log errors which may indicate a failure in the underlying
 *  resolver.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
LogError(char *func)
{
    int i;
    char *h, *e, buf[20];
#ifdef NEED_HERRNO
    extern int h_errno;
#endif

    i = h_errno;
    e = NULL;
    switch (i) {
    case HOST_NOT_FOUND:
        /* Log nothing. */
        return;
        break;

    case TRY_AGAIN:
        h = "temporary error - try again";
        break;

    case NO_RECOVERY:
        h = "unexpected server failure";
        break;

    case NO_DATA:
        h = "no valid IP address";
        break;

#ifdef NETDB_INTERNAL
    case NETDB_INTERNAL:
        h = "netdb internal error: ";
        e = strerror(errno);
        break;
#endif

    default:
        sprintf(buf, "unknown error #%d", i);
        h = buf;
    }

    Ns_Log(Error, "dns: %s failed: %s%s", func, h, e ? e : "");
}