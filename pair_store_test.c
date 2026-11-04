#include "pair_store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
    assert(argc==3);assert(pair_store_init(argv[1])==0);
    const btstack_link_key_db_t *db=pair_store_instance();
    bd_addr_t local={1,2,3,4,5,6},remote={9,8,7,6,5,4};
    link_key_t key;link_key_type_t type;
    db->set_local_bd_addr(local);assert(pair_store_ok());
    if(!strcmp(argv[2],"write")) {
        memset(key,0x5a,sizeof(key));db->put_link_key(remote,key,(link_key_type_t)4);assert(pair_store_count()==1);
    } else if(!strcmp(argv[2],"read")) {
        assert(db->get_link_key(remote,key,&type)==1);assert(type==4);
        for(unsigned i=0;i<sizeof(key);i++)assert(key[i]==0x5a);
    } else if(!strcmp(argv[2],"delete")) { db->delete_link_key(remote);assert(pair_store_count()==0); }
    else if(!strcmp(argv[2],"trusted")) assert(pair_store_is_trusted(remote));
    else if(!strcmp(argv[2],"untrusted")) assert(!pair_store_is_trusted(remote));
    else if(!strcmp(argv[2],"trust-only")) { pair_store_trust(remote);assert(pair_store_is_trusted(remote));assert(pair_store_count()==0); }
    else if(!strcmp(argv[2],"empty")) assert(db->get_link_key(remote,key,&type)==0);
    else return 64;
    puts("PASS: synthetic pairing operation; key contents not logged");return 0;
}
