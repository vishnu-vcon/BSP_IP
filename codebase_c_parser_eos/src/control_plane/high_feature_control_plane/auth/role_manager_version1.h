#ifndef ROLE_MANAGER_H
#define ROLE_MANAGER_H

#include <stdbool.h>

/* Check whether a role has permission to perform an action.
 * Returns true if allowed. */
bool check_permission(const char *role, const char *action);

/* Print all permissions for a role to stdout. */
void print_permissions(const char *role);

/* Fill perm_list with pointers to permission strings for role.
 * perm_list must hold at least max_perms pointers.
 * Returns the number of permissions written. */
int get_permissions(const char *role, const char **perm_list, int max_perms);

#endif /* ROLE_MANAGER_H */
