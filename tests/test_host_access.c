#include "host_access.h"
#include "md_test.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static MdSessionRequest req(unsigned n, int64_t expiry) {
    MdSessionRequest r = {.capabilities=MD_HOST_ACCESS_CAPS, .issued_at=100, .expires_at=expiry};
    snprintf(r.request_id,sizeof(r.request_id),"%032x",n); return r;
}
static void peer(unsigned n,char out[65]) { snprintf(out,65,"%064x",n); }
static MdHostAccessSnapshot view(MdHostAccess *a,int64_t now) {
    MdHostAccessSnapshot v; md_host_access_snapshot(a,now,&v); return v;
}
static void bounds_expiry(void) {
    MdHostAccess a; MD_CHECK(!md_host_access_init(&a));
    for (unsigned i=1;i<=16;i++) { char pk[65];peer(i,pk);MdSessionRequest r=req(i,110);MD_CHECK(!md_host_access_enqueue(&a,pk,&r,100)); }
    char pk[65];peer(17,pk);MdSessionRequest r=req(17,110);
    MD_CHECK(md_host_access_enqueue(&a,pk,&r,100)<0);
    MdHostAccessSnapshot old=view(&a,100); MD_CHECK(old.count==16);
    MdHostRequest selected;
    MD_CHECK(md_host_access_decide(&a,old.generation,old.revision,old.requests[0].request.request_id,true,false,1,7,110,&selected)<0);
    MD_CHECK(view(&a,110).count==0);
    MD_CHECK(!md_host_access_enqueue(&a,pk,&r,109));
    MD_CHECK(md_host_access_enqueue(&a,pk,&r,109)<0);
    md_host_access_destroy(&a);
}
typedef struct { MdHostAccess *a; MdHostAccessSnapshot v; bool approve; int rc; pthread_mutex_t *mu; pthread_cond_t *cv; int *ready; bool *go; } Race;
/* A portable start barrier (macOS has no pthread_barrier_t). */
static void *race(void *data) {
    Race *r=data;
    pthread_mutex_lock(r->mu); ++*r->ready; pthread_cond_broadcast(r->cv);
    while (!*r->go) pthread_cond_wait(r->cv,r->mu);
    pthread_mutex_unlock(r->mu);
    r->rc=md_host_access_decide(r->a,r->v.generation,r->v.revision,r->v.requests[0].request.request_id,r->approve,false,1,7,100,NULL);
    return NULL;
}
static void decision_race(void) {
    for (int i=0;i<100;i++) {
        MdHostAccess a;MD_CHECK(!md_host_access_init(&a));char pk[65];peer(1,pk);MdSessionRequest r=req(1,110);
        MD_CHECK(!md_host_access_enqueue(&a,pk,&r,100));
        pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
        int ready=0;bool go=false;
        Race x={.a=&a,.v=view(&a,100),.approve=true,.mu=&mu,.cv=&cv,.ready=&ready,.go=&go},y=x;y.approve=false;
        pthread_t t,u;MD_CHECK(!pthread_create(&t,NULL,race,&x));MD_CHECK(!pthread_create(&u,NULL,race,&y));
        pthread_mutex_lock(&mu);while(ready<2)pthread_cond_wait(&cv,&mu);go=true;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&mu);
        pthread_join(t,NULL);pthread_join(u,NULL);MD_CHECK((x.rc==0)+(y.rc==0)==1);
        pthread_cond_destroy(&cv);pthread_mutex_destroy(&mu);md_host_access_destroy(&a);
    }
}
static void decisions(void) {
    MdHostAccess a;MD_CHECK(!md_host_access_init(&a));char pk[65];peer(1,pk);MdSessionRequest r=req(1,110);
    MD_CHECK(!md_host_access_enqueue(&a,pk,&r,100));
    MdHostAccessSnapshot v=view(&a,100);MdHostRequest q;
    MD_CHECK(md_host_access_decide(&a,"old",v.revision,r.request_id,true,false,1,7,100,&q)<0);
    MD_CHECK(md_host_access_decide(&a,v.generation,v.revision,r.request_id,true,false,8,7,100,&q)<0);
    MD_CHECK(md_host_access_decide(&a,v.generation,v.revision,r.request_id,true,false,4,3,100,&q)<0);
    MD_CHECK(md_host_access_decide(&a,v.generation,v.revision,r.request_id,true,false,0,7,100,&q)<0);
    MD_CHECK(!md_host_access_decide(&a,v.generation,v.revision,r.request_id,true,true,1,7,100,&q));
    MD_CHECK(!md_host_access_take(&a,100,&q));
    MD_CHECK(md_host_access_remembered(&a,r.request_id,false,100)<0); // failed OK never approves
    v=view(&a,100);MD_CHECK(v.requests[0].state==MD_HOST_PENDING);
    MD_CHECK(!md_host_access_decide(&a,v.generation,v.revision,r.request_id,true,true,1,7,100,&q));
    md_host_access_cancel(&a,pk,r.request_id);
    MD_CHECK(md_host_access_remembered(&a,r.request_id,true,100)<0);
    MD_CHECK(!md_host_access_take(&a,100,&q));
    r=req(2,110);MD_CHECK(!md_host_access_enqueue(&a,pk,&r,100));v=view(&a,100);
    MD_CHECK(!md_host_access_decide(&a,v.generation,v.revision,r.request_id,true,false,1,7,100,&q));
    MD_CHECK(md_host_access_decide(&a,v.generation,v.revision,r.request_id,false,false,0,7,100,&q)<0);
    MD_CHECK(md_host_access_take(&a,100,&q)&&q.granted==1);
    char other[65];peer(2,other);md_host_access_cancel(&a,other,r.request_id);MD_CHECK(md_host_access_valid(&a,r.request_id));
    MD_CHECK(md_host_access_auto(&a,other,&r,1,100)<0);
    md_host_access_bound(&a,r.request_id,"session-id",1);v=view(&a,100);
    MD_CHECK(md_host_access_kick(&a,v.generation,v.revision,"wrong")<0);
    MD_CHECK(!md_host_access_kick(&a,v.generation,v.revision,"session-id"));MD_CHECK(!md_host_access_valid(&a,r.request_id));
    MD_CHECK(md_host_access_kick(&a,v.generation,v.revision,"session-id")<0);
    md_host_access_release(&a);MD_CHECK(!view(&a,100).occupied);
    r=req(3,110);MD_CHECK(!md_host_access_enqueue(&a,pk,&r,100));v=view(&a,100);
    MD_CHECK(!md_host_access_decide(&a,v.generation,v.revision,r.request_id,true,true,1,7,100,&q));
    MD_CHECK(md_host_access_remembered(&a,r.request_id,true,110)<0);
    MD_CHECK(!md_host_access_take(&a,110,&q));
    md_host_access_stop(&a);MD_CHECK(md_host_access_enqueue(&a,pk,&r,100)<0);
    md_host_access_destroy(&a);
}
static void revoke_peer(void) {
    MdHostAccess a; MD_CHECK(!md_host_access_init(&a));
    char first[65], other[65]; peer(1,first); peer(2,other);
    MdSessionRequest one=req(10,110), two=req(11,110);
    MD_CHECK(!md_host_access_enqueue(&a,first,&one,100));
    MD_CHECK(!md_host_access_enqueue(&a,other,&two,100));
    MdHostAccessSnapshot v=view(&a,100); MdHostRequest selected;
    MD_CHECK(!md_host_access_decide(&a,v.generation,v.revision,one.request_id,true,true,1,7,100,&selected));
    md_host_access_revoke_peer(&a,first);
    MD_CHECK(md_host_access_remembered(&a,one.request_id,true,100)<0);
    v=view(&a,100); MD_CHECK(v.count==1 && !strcmp(v.requests[0].peer,other));
    MD_CHECK(!md_host_access_auto(&a,first,&one,1,100));
    md_host_access_revoke_peer(&a,other); MD_CHECK(md_host_access_valid(&a,one.request_id));
    md_host_access_revoke_peer(&a,first); MD_CHECK(!md_host_access_valid(&a,one.request_id));
    MD_CHECK(view(&a,100).revoked); md_host_access_destroy(&a);
}
int main(void) {
    revoke_peer();
    bounds_expiry(); decisions(); decision_race();
    char id[33];MD_CHECK(!md_session_cancel_from_json("{\"type\":\"session_cancel\",\"v\":1,\"request_id\":\"00000000000000000000000000000001\"}",id));
    MD_CHECK(md_session_cancel_from_json("{\"type\":\"session_cancel\",\"v\":1,\"request_id\":\"bad\"}",id)<0);
    puts("PASS: bounds, strict deadline, caps, stale/duplicate decisions, approve/deny races, remember failure/cancel/expiry, one session, kick");
    return md_test_report();
}
