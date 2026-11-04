#ifndef PAIR_STORE_H
#define PAIR_STORE_H
#include <stddef.h>
#include "classic/btstack_link_key_db.h"
int pair_store_init(const char *directory);
int pair_store_ok(void);
size_t pair_store_count(void);
size_t pair_store_trusted_count(void);
int pair_store_is_trusted(const bd_addr_t address);
void pair_store_trust(const bd_addr_t address);
const btstack_link_key_db_t *pair_store_instance(void);
#endif
