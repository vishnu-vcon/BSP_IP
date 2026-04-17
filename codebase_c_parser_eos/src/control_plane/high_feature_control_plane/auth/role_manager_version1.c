/*
 * auth/role_manager.c
 *
 * Role-based permission map.
 * Roles: admin, operator, viewer
 */

#include "role_manager.h"
#include <stdio.h>
#include <string.h>

/* ---------- Permission tables ---------- */

static const char *admin_perms[] = {
    "secure-data",
    "reboot",
    "update-config",
    "start-stream",
    "stop-stream",
    "device:reboot",
    "device:network-reset",
    "device:factory-reset",
    "device:config-reset",
    "device:status",
    "manage:add-user",
    "manage:remove-user",   /* added: admin can delete users */
    NULL
};

static const char *operator_perms[] = {
    "secure-data",
    "start-stream",
    "stop-stream",
    "device:status",
    "device:network-reset",
    "device:config-reset",
    NULL
};

static const char *viewer_perms[] = {
    "secure-data",
    "start-stream",
    "stop-stream",
    "device:status",
    NULL
};

/* ---------- Internal lookup ---------- */

static const char **get_role_table(const char *role)
{
    if (strcmp(role, "admin")    == 0) return admin_perms;
    if (strcmp(role, "operator") == 0) return operator_perms;
    if (strcmp(role, "viewer")   == 0) return viewer_perms;
    return NULL;
}

/* ---------- Public API ---------- */

bool check_permission(const char *role, const char *action)
{
    const char **perms = get_role_table(role);
    if (!perms) return false;
    for (int i = 0; perms[i] != NULL; i++) {
        if (strcmp(perms[i], action) == 0) return true;
    }
    return false;
}

void print_permissions(const char *role)
{
    const char **perms = get_role_table(role);
    if (!perms) {
        printf("  [RoleMgr] unknown role '%s'\n", role);
        return;
    }
    printf("  [RoleMgr] permissions for '%s':\n", role);
    for (int i = 0; perms[i] != NULL; i++)
        printf("    - %s\n", perms[i]);
}

int get_permissions(const char *role, const char **perm_list, int max_perms)
{
    const char **perms = get_role_table(role);
    if (!perms) return 0;
    int count = 0;
    for (int i = 0; perms[i] != NULL && count < max_perms; i++)
        perm_list[count++] = perms[i];
    return count;
}
