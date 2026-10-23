#include "command_memory.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/resource.h>
#include <sys/proc.h>
bool sb_command_memory(pid_t group, uint64_t *bytes, sb_error *error) {
    *bytes = 0;
    if (group <= 0 || group == getpgrp()) {
        sb_fail(error, "Invalid command process group.");
        return false;
    }
    struct proc_bsdinfo leader = {0};
    int leader_size = proc_pidinfo(group, PROC_PIDTBSDINFO, 0, &leader, sizeof(leader));
    if (leader_size == (int)sizeof(leader) &&
        (leader.pbi_status == SZOMB || (leader.pbi_flags & PROC_FLAG_INEXIT)))
        return true;
    if (leader_size != (int)sizeof(leader) || leader.pbi_pgid != (uint32_t)group) {
        sb_fail(error, "Command leader is no longer in its owned process group.");
        return false;
    }
    /* Leave room for group growth between sizing and enumeration; fail closed
       at the bound rather than silently omit processes. */
    enum { CAP = 32768 };
    pid_t *pids = calloc(CAP, sizeof(*pids));
    if (!pids) {
        sb_fail(error, "Cannot allocate memory monitor process list.");
        return false;
    }
    int size = proc_listpids(PROC_PGRP_ONLY, (uint32_t)group, pids, CAP * (int)sizeof(*pids));
    bool ok = size > 0 && size < CAP * (int)sizeof(*pids) && size % (int)sizeof(*pids) == 0;
    for (int i = 0; ok && i < size / (int)sizeof(*pids); i++) {
        pid_t pid = pids[i];
        if (pid <= 0)
            continue;
        /* A short-lived child can exit between enumeration and either query. */
        struct proc_bsdinfo identity = {0};
        int got = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &identity, sizeof(identity));
        if (got != (int)sizeof(identity)) {
            if (errno == ESRCH || errno == ENOENT)
                continue;
            ok = false;
            break;
        }
        if (identity.pbi_pgid != (uint32_t)group)
            continue;
        if (identity.pbi_status == SZOMB || (identity.pbi_flags & PROC_FLAG_INEXIT))
            continue;
        struct rusage_info_v2 usage = {0};
        if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *)&usage)) {
            if (errno == ESRCH || errno == ENOENT)
                continue;
            ok = false;
            break;
        }
        if (getpgid(pid) != group)
            continue;
        if (UINT64_MAX - *bytes < usage.ri_phys_footprint) {
            ok = false;
            break;
        }
        *bytes += usage.ri_phys_footprint;
    }
    free(pids);
    if (!ok)
        sb_fail(error, "Cannot account for the complete command process group.");
    return ok;
}
#else
bool sb_command_memory(pid_t group, uint64_t *bytes, sb_error *error) {
    (void)group;
    (void)error;
    *bytes = 0;
    return true;
}
#endif
