/*
 * mock_faults.c - NVMPI_MOCK_FAULT / NVMPI_MOCK_DELAY_US parsing and
 * counters. See mock_faults.h for the contract.
 *
 * Thread-safety: g_mu guards the rule table, the cached spec and the
 * counters. It is a leaf lock (nothing is called while holding it except
 * libc string functions), so callers may hold ctx->mu.
 */
#define _GNU_SOURCE
#include "mock_faults.h"
#include "mock_log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_RULES  16
#define MAX_SPEC   255                /* longer specs are rejected whole   */
#define MAX_N      1000000000ul       /* sanity bound on the call number  */

struct rule {
    enum mock_fault_pt pt;
    unsigned long n;
    int sticky;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_spec[MAX_SPEC + 1];     /* spec the table was built from (the
                                       * first MAX_SPEC bytes when longer)  */
static size_t g_spec_len;             /* its full length                    */
static int  g_have_spec;
static struct rule g_rules[MAX_RULES];
static unsigned g_nrules;
static unsigned long g_calls[MFP_COUNT];

static const char *const k_names[MFP_COUNT] = {
    [MFP_DQBUF] = "dqbuf",   [MFP_QBUF] = "qbuf",
    [MFP_REQBUFS] = "reqbufs", [MFP_STREAMON] = "streamon",
    [MFP_EXPBUF] = "expbuf", [MFP_ALLOC] = "alloc",
    [MFP_OPEN] = "open",
};

/* Parse one "name@N[+]" token (already NUL-terminated, no commas). Returns
 * 0 and fills *r, or -1 for a malformed/unknown token. */
static int parse_token(const char *tok, struct rule *r)
{
    const char *at = strchr(tok, '@');
    char *end = NULL;
    size_t nlen;
    unsigned long n;
    int pt;

    if (!at || at == tok)
        return -1;
    nlen = (size_t)(at - tok);
    for (pt = 0; pt < MFP_COUNT; pt++)
        if (strlen(k_names[pt]) == nlen && strncmp(tok, k_names[pt], nlen) == 0)
            break;
    if (pt == MFP_COUNT)
        return -1;
    /* Digits only (strtoul would also accept a sign or leading blanks). */
    if (at[1] < '0' || at[1] > '9')
        return -1;
    n = strtoul(at + 1, &end, 10);
    if (n == 0 || n > MAX_N)
        return -1;
    r->sticky = 0;
    if (*end == '+') {
        r->sticky = 1;
        end++;
    }
    if (*end != '\0')
        return -1;
    r->pt = (enum mock_fault_pt)pt;
    r->n = n;
    return 0;
}

/* Rebuild the table when the environment changed. g_mu held. */
static void refresh_locked(void)
{
    const char *env = getenv("NVMPI_MOCK_FAULT");
    char buf[MAX_SPEC + 1], *tok, *save = NULL;
    size_t len;

    if (!env)
        env = "";
    len = strlen(env);
    if (g_have_spec && len == g_spec_len &&
        strncmp(env, g_spec, MAX_SPEC) == 0)
        return;                   /* unchanged: keep counters (and report
                                   * a rejected spec only once)            */
    g_nrules = 0;
    memset(g_calls, 0, sizeof(g_calls));
    snprintf(g_spec, sizeof(g_spec), "%.*s", MAX_SPEC, env);
    g_spec_len = len;
    g_have_spec = 1;
    if (len > MAX_SPEC) {
        MOCK_ERR("NVMPI_MOCK_FAULT longer than %d bytes: ignored", MAX_SPEC);
        return;
    }
    memcpy(buf, env, strlen(env) + 1);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        struct rule r;

        if (g_nrules == MAX_RULES) {
            MOCK_ERR("NVMPI_MOCK_FAULT: more than %d rules, rest ignored",
                     MAX_RULES);
            break;
        }
        if (parse_token(tok, &r) < 0)
            MOCK_ERR("NVMPI_MOCK_FAULT: ignoring bad token '%s'", tok);
        else
            g_rules[g_nrules++] = r;
    }
}

int mock_fault_hit(enum mock_fault_pt pt)
{
    int hit = 0;
    unsigned i;
    unsigned long call;

    if ((unsigned)pt >= MFP_COUNT)
        return 0;
    pthread_mutex_lock(&g_mu);
    refresh_locked();
    call = ++g_calls[pt];
    for (i = 0; i < g_nrules; i++)
        if (g_rules[i].pt == pt &&
            (call == g_rules[i].n ||
             (g_rules[i].sticky && call > g_rules[i].n)))
            hit = 1;
    pthread_mutex_unlock(&g_mu);
    if (hit)
        MOCK_TRACE("fault injected: %s call %lu", k_names[pt], call);
    return hit;
}

/* Sleep the microseconds named by env var `name` (no-op when unset or
 * malformed), capped at MOCK_DELAY_MAX_US. */
static void env_delay(const char *name)
{
    const char *env = getenv(name);
    char *end = NULL;
    unsigned long us;

    if (!env || !*env || *env < '0' || *env > '9')
        return;
    us = strtoul(env, &end, 10);
    if (*end != '\0')
        return;                       /* trailing junk: treat as unset     */
    if (us > MOCK_DELAY_MAX_US)
        us = MOCK_DELAY_MAX_US;
    if (us)
        usleep((useconds_t)us);
}

void mock_fault_delay(void)
{
    env_delay("NVMPI_MOCK_DELAY_US");
}

void mock_fault_ctrl_delay(void)
{
    env_delay("NVMPI_MOCK_CTRL_DELAY_US");
}
