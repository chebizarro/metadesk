#include "nostr.h"
#include "md_test.h"
#include <string.h>
static const char *sk="0000000000000000000000000000000000000000000000000000000000000001";
static const char *host="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
int main(int argc,char **argv) {
    MD_CHECK(argc==2 || argc==3); const char *urls[]={argv[1]};
    MdNostrConfig cfg={.sk_hex=sk,.relay_urls=urls,.relay_count=1,.quality_preferences=true};
    MdNostr *n=md_nostr_create(&cfg,NULL); MD_CHECK(n);
    MdQuality q={1280,720,1000000,30}, got={0};
    MD_CHECK(md_nostr_quality_load(n,host,&got)==1);
    if (argc==3) {
        MD_CHECK(md_nostr_quality_save(n,host,&q)<0); md_nostr_destroy(n);
        n=md_nostr_create(&cfg,NULL); MD_CHECK(n);
        MD_CHECK(md_nostr_quality_load(n,host,&got)==1); md_nostr_destroy(n); md_nostr_shutdown();
        return md_test_report();
    }
    MD_CHECK(!md_nostr_quality_save(n,host,&q));
    md_nostr_destroy(n);
    n=md_nostr_create(&cfg,NULL); MD_CHECK(n);
    MD_CHECK(!md_nostr_quality_load(n,host,&got));
    MD_CHECK(!memcmp(&q,&got,sizeof(q)));
    MdQuality other={640,480,500000,15};
    MD_CHECK(!md_nostr_quality_save(n,"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",&other));
    md_nostr_destroy(n); n=md_nostr_create(&cfg,NULL); MD_CHECK(n);
    MD_CHECK(!md_nostr_quality_load(n,host,&got) && !memcmp(&q,&got,sizeof(q)));
    MD_CHECK(!md_nostr_quality_load(n,"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",&got));
    MD_CHECK(!memcmp(&got,&other,sizeof(got))); md_nostr_destroy(n); md_nostr_shutdown();
    return md_test_report();
}
